// Access to the board: CLI command lines sent through the relay
// (POST /api/cli, tools/bench/server). The only module that knows the CLI
// syntax; the reply types mirror the json mode of the firmware.

/** Reply of the board with "ok" not true. */
export class CliError extends Error {
  readonly line: string
  readonly reply: Record<string, unknown>
  constructor(line: string, reply: Record<string, unknown>) {
    super(`${line}: ${String(reply.error ?? "invalid reply")}`)
    this.line = line
    this.reply = reply
  }
}

/** The relay could not get a reply (port absent, no answer...). */
export class RelayError extends Error {
  readonly status: number
  constructor(status: number, detail: string) {
    super(`relay ${status}: ${detail}`)
    this.status = status
  }
}

export type Reply = { ok: boolean; error?: string; [key: string]: unknown }

type Options = { replyBytes?: number; timeout?: number }

/** Send a line, return the reply as is, ok or not (console). */
export async function cliRaw(line: string, opts: Options = {}): Promise<Reply> {
  const res = await fetch("/api/cli", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      line,
      reply_bytes: opts.replyBytes ?? 0,
      timeout: opts.timeout ?? null,
    }),
  })
  const body = await res.json().catch(() => ({}))
  if (!res.ok) {
    const detail = body.detail
    throw new RelayError(
      res.status,
      typeof detail === "string" ? detail : res.statusText
    )
  }
  return body as Reply
}

/** Send a line, return the reply; throws CliError if it is not ok. */
export async function cli<T>(line: string, opts: Options = {}): Promise<T> {
  const reply = await cliRaw(line, opts)
  if (reply.ok !== true) {
    throw new CliError(line, reply)
  }
  return reply as T
}

// GPIO --------------------------------------------------------------------

export type PinMode = "IN" | "OUT" | "AF" | "ANL"

export type GpioPin = {
  pin: number
  mode: PinMode
  pull: string
  level: number
  af: string // "-" when not in AF mode
}

export type GpioPort = { port: string; clock: boolean; pins: GpioPin[] }

export type GpioShow = { ports: GpioPort[] }

export const gpioShow = () => cli<GpioShow>("gpio show")

/** Pins by name ("PA5"); the pins of ports without clock are absent. */
export function pinsByName(show: GpioShow): Map<string, GpioPin> {
  const pins = new Map<string, GpioPin>()
  for (const port of show.ports) {
    for (const pin of port.pins) {
      pins.set(`P${port.port.at(-1)}${pin.pin}`, pin)
    }
  }
  return pins
}

/** Only on a pin configured as output. */
export const gpioSet = (pin: string, level: 0 | 1) =>
  cli<{ pin: string; level: number }>(`gpio set ${pin} ${level}`)

// Analog ------------------------------------------------------------------

/** Single conversion of a software-triggered ADC. */
export const adcRead = (n: number, ch: number) =>
  cli<{ raw: number; mv: number }>(`adc ${n} ${ch}`)

export const dac = (n: number, ch: number, mv: number) =>
  cli<{ raw: number; mv: number }>(`dac ${n} ${ch} ${mv}`)

export type Shape = "sine" | "square" | "step"

export const gen = (shape: Shape, f: number, vpp: number) =>
  cli<{ shape: Shape; f: number; vpp: number }>(
    shape === "step" ? `gen step ${vpp}` : `gen ${shape} ${f} ${vpp}`
  )

export type Acq = { fs: number; f: number; n: number }

export const acq = (fs: number, n: number) =>
  // the acquisition lasts n / fs, plus the settling of the circuit
  cli<Acq>(`acq ${fs} ${n}`, { timeout: n / fs + 2 })

export type Dump = {
  fs: number
  n: number
  f: number
  shape: Shape
  vpp: number
  vref: number // mV
  full: number // full scale, raw
  in_label: string
  out_label: string
  in: number[] // raw
  out: number[]
}

export const dump = (n: number) =>
  // up to "4095," per value, 2 values per point
  cli<Dump>("dump", { replyBytes: 10 * n + 300 })
