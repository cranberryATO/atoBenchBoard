"""Drive the STM32 analog scope through its CLI and plot the curves.

The board generates a waveform on the DAC, samples the input and the output
of the circuit under test with the ADC, and dumps the points. The CLI is put
in json mode: each command gets one JSON object on one line, whose "ok"
member tells if the rest is valid:

    {"fs":20000.000,"n":2000,...,"in":[1023,...],"out":[1022,...],"ok":true}

Examples:
    uv run scope.py --port /dev/ttyACM0 --shape step --vpp 2000 \
        --fs 20000 --n 2000 --rc 10e3 100e-9
    uv run scope.py --port /dev/ttyACM0 --shape sine --freq 160 --vpp 2000 \
        --fs 20000 --n 2000 --rc 10e3 100e-9
"""

import argparse
import csv
import json
import math
import sys

import matplotlib.pyplot as plt
import numpy as np
import serial


class Cli:
    """Command / reply exchange with the CLI, in json mode."""

    def __init__(self, port: str, baudrate: int, timeout: float):
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.baudrate = baudrate
        # flush a partially typed line and any pending output
        self.ser.write(b"\r")
        self.ser.reset_input_buffer()
        # still in plain mode until this command: skip its echo
        self.ser.write(b"mode json\r")
        while True:
            reply = self._parse(self._readline())
            if reply is not None and reply.get("mode") == "json":
                break

    def _readline(self) -> str:
        line = self.ser.readline()
        if not line:
            raise TimeoutError("no answer from the board")
        return line.decode("ascii", errors="replace").strip()

    @staticmethod
    def _parse(line: str) -> dict | None:
        try:
            reply = json.loads(line)
        except json.JSONDecodeError:
            return None
        return reply if isinstance(reply, dict) else None

    def command(self, cmd: str, reply_bytes: int = 0) -> dict:
        """Send `cmd`, return its reply; raise if it is not ok.

        `reply_bytes`: expected size of the reply, to extend the timeout
        for long transfers (the timeout covers the whole line)."""
        self.ser.reset_input_buffer()
        timeout = self.ser.timeout
        self.ser.timeout = timeout + 1.5 * reply_bytes * 10 / self.baudrate
        try:
            self.ser.write((cmd + "\r").encode("ascii"))
            line = self._readline()
        finally:
            self.ser.timeout = timeout
        reply = self._parse(line)
        if reply is None:
            raise RuntimeError(f"{cmd!r}: not a JSON reply: {line[:80]!r}")
        if reply.get("ok") is not True:
            raise RuntimeError(f"{cmd!r}: {reply.get('error', 'invalid reply')}")
        return reply

    def dump(self, n: int) -> tuple[dict, np.ndarray]:
        # up to "4095," per value, 2 values per point
        reply = self.command("dump", reply_bytes=10 * n + 300)
        data = np.column_stack([reply.pop("in"), reply.pop("out")]).astype(float)
        if len(data) != reply["n"]:
            raise RuntimeError(f"expected {reply['n']} points, got {len(data)}")
        return reply, data


def rc_lowpass(vin: np.ndarray, v0: float, dt: float, rc: float) -> np.ndarray:
    """Exact first order low-pass response to a piecewise constant input."""
    k = 1.0 - math.exp(-dt / rc)
    out = np.empty_like(vin)
    y = v0
    for i, x in enumerate(vin):
        out[i] = y
        y += k * (x - y)
    return out


def sine_fit(t: np.ndarray, v: np.ndarray, f: float) -> tuple[float, float]:
    """Amplitude and phase (rad) of the component at f (least squares)."""
    w = 2 * math.pi * f
    m = np.column_stack([np.ones_like(t), np.sin(w * t), np.cos(w * t)])
    _, a, b = np.linalg.lstsq(m, v, rcond=None)[0]
    return math.hypot(a, b), math.atan2(b, a)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", required=True, help="CLI serial port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--shape", choices=["step", "sine", "square"], default="step")
    p.add_argument("--freq", type=int, default=100, help="Hz (sine, square)")
    p.add_argument("--vpp", type=int, default=2000, help="mV peak to peak")
    p.add_argument("--fs", type=int, default=20000, help="sample rate, Hz")
    p.add_argument("--n", type=int, default=2000, help="number of points")
    p.add_argument("--rc", nargs=2, type=float, metavar=("R", "C"),
                   help="overlay the theory of an RC low-pass (ohm, farad)")
    p.add_argument("--save", metavar="FILE", help="also save t,vin,vout as CSV")
    args = p.parse_args()

    # the acquisition itself lasts n / fs, plus the settling delay
    cli = Cli(args.port, args.baud, timeout=args.n / args.fs + 3)
    if args.shape == "step":
        cli.command(f"gen step {args.vpp}")
    else:
        cli.command(f"gen {args.shape} {args.freq} {args.vpp}")
    print(cli.command(f"acq {args.fs} {args.n}"))
    header, data = cli.dump(args.n)

    fs = header["fs"]
    f = header["f"]
    to_volt = header["vref"] / header["full"] / 1000.0
    vin, vout = data[:, 0] * to_volt, data[:, 1] * to_volt
    t = np.arange(len(vin)) / fs

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(t * 1e3, vin, label=f"input ({header['in_label']})")
    ax.plot(t * 1e3, vout, label=f"output ({header['out_label']})")
    title = f"{header['shape']}, fs = {fs:g} Hz"
    if f > 0:
        title += f", f = {f:g} Hz"

    if args.rc:
        r, c = args.rc
        rc = r * c
        theory = rc_lowpass(vin, vout[0], 1.0 / fs, rc)
        ax.plot(t * 1e3, theory, "--", label="RC low-pass theory")
        title += f"\nRC = {rc * 1e3:g} ms, fc = {1 / (2 * math.pi * rc):.1f} Hz"
        if header["shape"] == "sine":
            a_in, ph_in = sine_fit(t, vin, f)
            a_out, ph_out = sine_fit(t, vout, f)
            wrc = 2 * math.pi * f * rc
            gain_th = 1 / math.sqrt(1 + wrc**2)
            phase_th = -math.degrees(math.atan(wrc))
            phase = math.degrees(math.remainder(ph_out - ph_in, 2 * math.pi))
            title += (f"\ngain {a_out / a_in:.3f} (theory {gain_th:.3f}), "
                      f"phase {phase:.1f}° (theory {phase_th:.1f}°)")

    ax.set_title(title)
    ax.set_xlabel("time (ms)")
    ax.set_ylabel("voltage (V)")
    ax.grid(True)
    ax.legend()

    if args.save:
        with open(args.save, "w", newline="") as fp:
            w = csv.writer(fp)
            w.writerow(["t", "vin", "vout"])
            w.writerows(zip(t, vin, vout))

    plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
