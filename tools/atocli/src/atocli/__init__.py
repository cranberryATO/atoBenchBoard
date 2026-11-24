"""Access to the atoBenchBoard CLI from Python.

The CLI is put in json mode: no echo, no prompt, and each command line gets
exactly one JSON object, on one line, whose last member "ok" tells if the
rest of the object is valid (see Inc/ato_cli_core.h):

    {"raw":1240,"mv":999,"ok":true}
    {"error":"<fs>: out of range 10..200000","ok":false}

Example:
    with Cli("/dev/ttyUSB0") as cli:
        print(cli.command("ps")["tasks"])
"""

import json

import serial

__all__ = ["Cli", "CliError"]


class CliError(RuntimeError):
    """A reply with "ok" not true; `reply` holds the whole reply."""

    def __init__(self, cmd: str, reply: dict):
        super().__init__(f"{cmd!r}: {reply.get('error', 'invalid reply')}")
        self.cmd = cmd
        self.reply = reply


class Cli:
    """Command / reply exchange with the CLI, in json mode."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 2.0):
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.baudrate = baudrate
        # flush a partially typed line and any pending output
        self.ser.write(b"\r")
        self.ser.reset_input_buffer()
        # in plain mode until this command: skip its echo and the prompt
        self.ser.write(b"mode json\r")
        while True:
            reply = self._parse(self._readline())
            if reply is not None and reply.get("mode") == "json":
                break

    def close(self) -> None:
        self.ser.close()

    def __enter__(self) -> "Cli":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

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

    def request(self, cmd: str, reply_bytes: int = 0,
                timeout: float | None = None) -> dict:
        """Send `cmd`, return its reply, ok or not.

        The serial timeout covers the whole reply line: `reply_bytes` (its
        expected size) extends it for long transfers, `timeout` replaces
        the base timeout (e.g. for a long acquisition)."""
        self.ser.reset_input_buffer()
        saved = self.ser.timeout
        base = saved if timeout is None else timeout
        self.ser.timeout = base + 1.5 * reply_bytes * 10 / self.baudrate
        try:
            self.ser.write((cmd + "\r").encode("ascii"))
            line = self._readline()
        finally:
            self.ser.timeout = saved
        reply = self._parse(line)
        if reply is None or "ok" not in reply:
            raise RuntimeError(f"{cmd!r}: not a JSON reply: {line[:80]!r}")
        return reply

    def command(self, cmd: str, reply_bytes: int = 0,
                timeout: float | None = None) -> dict:
        """Send `cmd`, return its reply; raise CliError if it is not ok."""
        reply = self.request(cmd, reply_bytes, timeout)
        if reply["ok"] is not True:
            raise CliError(cmd, reply)
        return reply
