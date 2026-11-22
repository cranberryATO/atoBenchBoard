"""Drive the STM32 analog scope through its CLI and plot the curves.

The board generates a waveform on the DAC, samples the input and the output
of the circuit under test with the ADC, and dumps the points as hex CSV:

    #scope fs=20000.000 n=2000 f=0.000 shape=step vpp=2000 vref=3300 ...
    3FF,400
    ...
    #end

Examples:
    uv run scope.py --port /dev/ttyACM0 --shape step --vpp 2000 \
        --fs 20000 --n 2000 --rc 10e3 100e-9
    uv run scope.py --port /dev/ttyACM0 --shape sine --freq 160 --vpp 2000 \
        --fs 20000 --n 2000 --rc 10e3 100e-9
"""

import argparse
import csv
import math
import sys

import matplotlib.pyplot as plt
import numpy as np
import serial


class Scope:
    """Command / response exchange with the CLI.

    Every analog command answers one line starting with "ok" or "error:";
    the echo of the command and any other text are ignored.
    """

    def __init__(self, port: str, baudrate: int, timeout: float):
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        # flush a partially typed line and any pending output
        self.ser.write(b"\r")
        self.ser.reset_input_buffer()

    def _readline(self) -> str:
        line = self.ser.readline()
        if not line:
            raise TimeoutError("no answer from the board")
        return line.decode("ascii", errors="replace").strip()

    def command(self, cmd: str) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r").encode("ascii"))
        while True:
            line = self._readline()
            if line.startswith("ok"):
                return line
            if line.startswith("error:"):
                raise RuntimeError(f"{cmd!r}: {line}")

    def dump(self) -> tuple[dict, np.ndarray]:
        self.ser.reset_input_buffer()
        self.ser.write(b"dump\r")
        while True:
            line = self._readline()
            if line.startswith("error:"):
                raise RuntimeError(f"'dump': {line}")
            if line.startswith("#scope"):
                break
        header = dict(item.split("=", 1) for item in line.split()[1:])
        points = []
        while True:
            line = self._readline()
            if line == "#end":
                break
            a, b = line.split(",")
            points.append((int(a, 16), int(b, 16)))
        data = np.array(points, dtype=float)
        if len(data) != int(header["n"]):
            raise RuntimeError(f"expected {header['n']} points, got {len(data)}")
        return header, data


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
    scope = Scope(args.port, args.baud, timeout=args.n / args.fs + 3)
    if args.shape == "step":
        scope.command(f"gen step {args.vpp}")
    else:
        scope.command(f"gen {args.shape} {args.freq} {args.vpp}")
    print(scope.command(f"acq {args.fs} {args.n}"))
    header, data = scope.dump()

    fs = float(header["fs"])
    f = float(header["f"])
    to_volt = float(header["vref"]) / float(header["full"]) / 1000.0
    vin, vout = data[:, 0] * to_volt, data[:, 1] * to_volt
    t = np.arange(len(vin)) / fs

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(t * 1e3, vin, label=f"input ({header['in']})")
    ax.plot(t * 1e3, vout, label=f"output ({header['out']})")
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
