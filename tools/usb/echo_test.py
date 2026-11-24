# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""Test the CDC interface of the board, with its temporary echo task.

Sends blocks of 1 to 300 bytes, and larger ones, among which the multiples of
64 bytes (a full-speed USB packet: a transfer of such a size needs a
zero-length packet to end), and checks that each one comes back unchanged.

Example:
    uv run tools/usb/echo_test.py /dev/atobench
"""

import argparse
import os
import sys
import time

import serial

SIZES = list(range(1, 301)) + [384, 448, 512, 1000, 1024, 2048, 4096]


def echo(ser: serial.Serial, size: int, timeout: float) -> tuple[bool, float]:
    """Send `size` random bytes; True if the same bytes come back."""
    data = os.urandom(size)
    start = time.monotonic()
    ser.write(data)
    received = bytearray()
    while len(received) < size and time.monotonic() - start < timeout:
        received += ser.read(size - len(received))
    return bytes(received) == data, time.monotonic() - start


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("port", help="CDC port, e.g. /dev/atobench")
    p.add_argument("--timeout", type=float, default=1.0,
                   help="max time for one block, in seconds")
    args = p.parse_args()

    with serial.Serial(args.port, timeout=0.05) as ser:  # DTR on: "connected"
        time.sleep(0.1)
        ser.reset_input_buffer()
        failures = []
        worst = (0.0, 0)
        for size in SIZES:
            ok, duration = echo(ser, size, args.timeout)
            if not ok:
                failures.append(size)
                ser.reset_input_buffer()
            elif duration > worst[0]:
                worst = (duration, size)

        # throughput: 64 KB in 4 KB blocks
        total, start = 0, time.monotonic()
        for _ in range(16):
            ok, _ = echo(ser, 4096, args.timeout)
            if not ok:
                failures.append("throughput")
                break
            total += 4096
        rate = total / (time.monotonic() - start) / 1024

    multiples = [s for s in SIZES if s % 64 == 0]
    print(f"{len(SIZES)} block sizes, of which multiples of 64: {multiples}")
    print(f"slowest echo: {worst[0] * 1000:.1f} ms ({worst[1]} bytes)")
    print(f"round-trip throughput: {rate:.0f} KB/s")
    if failures:
        print(f"FAILED for sizes: {failures}")
        return 1
    print("all echoes OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
