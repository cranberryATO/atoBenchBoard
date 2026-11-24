# atoBench: web UI of the atoBenchBoard

    browser (React/TS)  --POST /api/cli-->  relay (FastAPI)  --atocli-->  UART  -->  STM32

- `server/`: the relay. A single route, `POST /api/cli {"line": "gpio show"}`,
  returns the reply of the CLI (json mode) as is. It knows no command. It
  owns the serial port: pytest and `scope.py` cannot use it meanwhile.
- `ui/`: React + TypeScript, Vite, Tailwind v4, shadcn/ui (Base UI).
  `src/cli.ts` is the only module that knows the CLI syntax.
  Pages: Pinout (package view), IOs (GPIO, DAC, generator and acquisition),
  Console (any command line).
- `ui/src/data/pinout.json`: generated from the CubeMX database
  (from the project root):

      uv run tools/pinout/gen_pinout.py -o tools/bench/ui/src/data/pinout.json

## Development

Two terminals:

    cd tools/bench/server
    ATO_CLI_PORT=/dev/ttyUSB0 uv run fastapi dev app.py     # port 8000

    cd tools/bench/ui
    npm install       # once
    npm run dev       # http://localhost:5173, /api proxied to port 8000

## Deployment (e.g. a Raspberry Pi)

Build the UI on the PC (`npm run build` in `ui/`) and copy `tools/` to the
Pi; the relay then also serves `ui/dist`, no Node.js needed on the Pi:

    cd tools/bench/server
    ATO_CLI_PORT=/dev/ttyUSB0 uv run fastapi run app.py     # http://<pi>:8000
