"""Web relay of the atoBenchBoard CLI.

A single route, POST /api/cli {"line": "gpio show"}: the line is sent to the
CLI (json mode) and its reply returned as is, {"...": ..., "ok": true|false};
the UI interprets it. The relay knows no command: it does not change when
commands are added to the firmware.

HTTP status: 200 when the board replied (even with "ok": false); 503 when
the port is not available (reconnection at the next request); 504 when the
board does not answer; 502 when the reply is not JSON.

Environment: ATO_CLI_PORT (e.g. /dev/ttyUSB0), ATO_CLI_BAUD (default: the
one of atocli, i.e. of the firmware).

The UI, once built (tools/bench/ui/dist), is served under "/".

Run (from this directory):
    ATO_CLI_PORT=/dev/ttyUSB0 uv run fastapi dev app.py
"""

import asyncio
import os
from contextlib import asynccontextmanager
from pathlib import Path

from atocli import DEFAULT_BAUDRATE, Cli
from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

UI_DIST = Path(__file__).resolve().parent.parent / "ui" / "dist"


class Board:
    """The CLI connection: opened on demand, one command at a time."""

    def __init__(self, port: str | None, baudrate: int):
        self.port = port
        self.baudrate = baudrate
        self.cli: Cli | None = None
        self.lock = asyncio.Lock()

    def close(self) -> None:
        if self.cli is not None:
            self.cli.close()
            self.cli = None

    def _request(self, line: str, reply_bytes: int,
                 timeout: float | None) -> dict:
        if self.port is None:
            raise HTTPException(503, "ATO_CLI_PORT is not set")
        try:
            if self.cli is None:
                self.cli = Cli(self.port, self.baudrate)
            return self.cli.request(line, reply_bytes, timeout)
        except TimeoutError as e:  # an OSError: tested first
            if self.cli is None:  # no answer to "mode json"
                raise HTTPException(503, f"{self.port}: {e}") from e
            raise HTTPException(504, f"{line!r}: {e}") from e
        except OSError as e:  # port absent or unplugged: reopen next time
            self.close()
            raise HTTPException(503, f"{self.port}: {e}") from e
        except RuntimeError as e:  # not a JSON reply
            raise HTTPException(502, str(e)) from e

    async def request(self, line: str, reply_bytes: int = 0,
                      timeout: float | None = None) -> dict:
        async with self.lock:  # pyserial is blocking: run in a thread
            return await asyncio.to_thread(self._request, line, reply_bytes,
                                           timeout)


board = Board(os.environ.get("ATO_CLI_PORT"),
              int(os.environ.get("ATO_CLI_BAUD", DEFAULT_BAUDRATE)))


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    board.close()


app = FastAPI(title="atoBench", lifespan=lifespan)


class CliRequest(BaseModel):
    line: str = Field(max_length=200, examples=["gpio show"])
    # expected size of the reply, to extend the timeout (e.g. "dump")
    reply_bytes: int = Field(0, ge=0, le=1_000_000)
    # base timeout in seconds (e.g. a long "acq"); default: the one of atocli
    timeout: float | None = Field(None, gt=0, le=120)


@app.post("/api/cli")
async def cli(req: CliRequest) -> dict:
    """Run a CLI command line, return the reply of the board as is."""
    return await board.request(req.line, req.reply_bytes, req.timeout)


if UI_DIST.is_dir():  # after the API routes, which take precedence
    app.mount("/", StaticFiles(directory=UI_DIST, html=True), name="ui")
