import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"
import { toast } from "sonner"

import {
  adcList,
  adcRead,
  dacList,
  dac,
  gpioSet,
  gpioShow,
  pinsByName,
  type DacOutput,
  type GpioPin,
} from "@/cli"
import { Sparkline } from "@/components/Sparkline"
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { Slider } from "@/components/ui/slider"
import { Switch } from "@/components/ui/switch"
import pinout from "@/data/pinout.json"
import { HISTORY_LENGTH, pushSample, samples } from "@/lib/history"

const POLL_MS = 250
const VREF_MV = 3300 // full scale of the ADC graphs

const onError = (e: Error) => toast.error(e.message)

type AdcChannel = { n: number; ch: number }
type AdcSample = AdcChannel & { mv?: number; error?: string }

const gpioKey = (pin: string) => `gpio:${pin}`
const adcKey = (a: AdcChannel) => `adc:${a.n}:${a.ch}`
const adcName = (a: AdcChannel) => `ADC${a.n} IN${a.ch}`
const dacKey = (d: DacOutput) => `dac:${d.dac}:${d.ch}`
const dacName = (d: DacOutput) => `DAC${d.dac} OUT${d.ch}`

// GPIO of an analog signal ("ADC3_IN1" -> "PB1"), from the CubeMX database;
// none for an internal channel (temperature, VREFINT...)
const analogPins: Record<string, string> = pinout.analog
const adcPin = (a: AdcChannel) => analogPins[`ADC${a.n}_IN${a.ch}`]
const dacPin = (d: DacOutput) => analogPins[`DAC${d.dac}_OUT${d.ch}`]

/** Channel name, and its pin in muted text. */
function ChannelLabel({ name, pin }: { name: string; pin?: string }) {
  return (
    <span className={LABEL}>
      {name}
      {pin && <span className="ml-1.5 text-muted-foreground">{pin}</span>}
    </span>
  )
}

/*
 * One poll: the GPIO configuration and levels, then each ADC channel; the
 * samples of the inputs and outputs are appended to their history
 * (scrolling graphs).
 */
async function poll(adcs: AdcChannel[]) {
  const pins = pinsByName(await gpioShow())
  for (const [name, p] of pins) {
    if (p.mode === "IN" || p.mode === "OUT") {
      pushSample(gpioKey(name), p.level)
    }
  }
  const adcSamples: AdcSample[] = []
  for (const a of adcs) {
    try {
      const { mv } = await adcRead(a.n, a.ch)
      pushSample(adcKey(a), mv)
      adcSamples.push({ ...a, mv })
    } catch (e) {
      pushSample(adcKey(a), NaN)
      adcSamples.push({ ...a, error: (e as Error).message })
    }
  }
  let dacs: DacOutput[] = []
  let dacError: string | undefined
  try {
    dacs = (await dacList()).outputs
    for (const d of dacs) {
      if (d.enabled) {
        pushSample(dacKey(d), d.mv)
      }
    }
  } catch (e) {
    dacError = (e as Error).message
  }
  return { pins, adcSamples, dacs, dacError }
}

function pinsInMode(pins: Map<string, GpioPin>, mode: GpioPin["mode"]) {
  return [...pins].filter(([, p]) => p.mode === mode)
}

const seconds = (HISTORY_LENGTH * POLL_MS) / 1000

// same label width for all the graphs: they share the same time axis
const LABEL = "w-28 shrink-0"

function GpioRows({
  pins,
  color,
}: {
  pins: [string, GpioPin][]
  color: string
}) {
  return pins.map(([name, p]) => (
    <div key={name} className="flex items-center gap-3 text-sm">
      <span className={LABEL}>{name}</span>
      <Sparkline
        values={samples(gpioKey(name))}
        min={0}
        max={1}
        step
        color={color}
        title={`${name}: ${p.level}`}
      />
    </div>
  ))
}

// Inputs and outputs on the same time axis: e.g. an output wired to an
// input shows both levels one above the other.
function GpioLevels({ pins }: { pins: Map<string, GpioPin> }) {
  const inputs = pinsInMode(pins, "IN")
  const outputs = pinsInMode(pins, "OUT")
  return (
    <Card>
      <CardHeader>
        <CardTitle>GPIO levels</CardTitle>
      </CardHeader>
      <CardContent className="flex flex-col gap-1.5">
        <div className="text-xs text-muted-foreground">Inputs</div>
        {inputs.length === 0 && (
          <p className="text-sm text-muted-foreground">No pin in input mode.</p>
        )}
        <GpioRows pins={inputs} color="var(--pin-in)" />
        <div className="mt-2 text-xs text-muted-foreground">Outputs</div>
        {outputs.length === 0 && (
          <p className="text-sm text-muted-foreground">
            No pin in output mode.
          </p>
        )}
        <GpioRows pins={outputs} color="var(--pin-out)" />
      </CardContent>
    </Card>
  )
}

// ADC inputs and DAC outputs on the same time axis and scale: e.g. a DAC
// output wired to an ADC input shows both values one above the other.
function AnalogLevels({
  adcs,
  adcSamples,
  adcError,
  dacs,
  dacError,
}: {
  adcs: AdcChannel[]
  adcSamples: AdcSample[]
  adcError?: string
  dacs: DacOutput[]
  dacError?: string
}) {
  return (
    <Card>
      <CardHeader>
        <CardTitle>Analog levels</CardTitle>
      </CardHeader>
      <CardContent className="flex flex-col gap-1.5">
        <div className="text-xs text-muted-foreground">ADC inputs</div>
        {adcError && <p className="text-sm text-destructive">{adcError}</p>}
        {!adcError && adcs.length === 0 && (
          <p className="text-sm text-muted-foreground">
            No software-triggered ADC channel.
          </p>
        )}
        {adcs.map((a) => {
          const s = adcSamples.find((x) => x.n === a.n && x.ch === a.ch)
          return (
            <div key={adcKey(a)} className="flex items-center gap-3 text-sm">
              <ChannelLabel name={adcName(a)} pin={adcPin(a)} />
              {s?.error ? (
                <span className="min-w-0 flex-1 truncate text-destructive">
                  {s.error}
                </span>
              ) : (
                <Sparkline
                  values={samples(adcKey(a))}
                  min={0}
                  max={VREF_MV}
                  color="var(--pin-anl)"
                  title={
                    s?.mv !== undefined
                      ? `${adcName(a)}: ${s.mv} mV`
                      : undefined
                  }
                />
              )}
            </div>
          )
        })}

        <div className="mt-2 text-xs text-muted-foreground">DAC outputs</div>
        {dacError && <p className="text-sm text-destructive">{dacError}</p>}
        {!dacError && dacs.every((d) => !d.enabled) && (
          <p className="text-sm text-muted-foreground">
            No enabled DAC channel.
          </p>
        )}
        {dacs
          .filter((d) => d.enabled)
          .map((d) => (
            <div key={dacKey(d)} className="flex items-center gap-3 text-sm">
              <ChannelLabel name={dacName(d)} pin={dacPin(d)} />
              <Sparkline
                values={samples(dacKey(d))}
                min={0}
                max={VREF_MV}
                color="var(--pin-out)"
                title={`${dacName(d)}: ${d.mv} mV`}
              />
            </div>
          ))}
      </CardContent>
    </Card>
  )
}

function GpioOutputs({ pins }: { pins: Map<string, GpioPin> }) {
  const queryClient = useQueryClient()
  const set = useMutation({
    mutationFn: ({ pin, level }: { pin: string; level: 0 | 1 }) =>
      gpioSet(pin, level),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ["ios"] }),
    onError,
  })
  const outputs = pinsInMode(pins, "OUT")
  return (
    <Card>
      <CardHeader>
        <CardTitle>GPIO outputs</CardTitle>
      </CardHeader>
      <CardContent className="flex flex-col gap-3">
        {outputs.length === 0 && (
          <p className="text-sm text-muted-foreground">
            No pin in output mode.
          </p>
        )}
        {outputs.map(([name, p]) => (
          <div key={name} className="flex items-center gap-3">
            <Switch
              id={`out-${name}`}
              checked={p.level === 1}
              disabled={set.isPending}
              onCheckedChange={(checked) =>
                set.mutate({ pin: name, level: checked ? 1 : 0 })
              }
            />
            <Label htmlFor={`out-${name}`}>{name}</Label>
          </div>
        ))}
      </CardContent>
    </Card>
  )
}

function DacRow({ d }: { d: DacOutput }) {
  const queryClient = useQueryClient()
  // value being edited, until the board has applied it (next poll)
  const [draft, setDraft] = useState<number | null>(null)
  const set = useMutation({
    mutationFn: (mv: number) => dac(d.dac, d.ch, mv),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ["ios"] })
      setDraft(null)
    },
    onError: (e: Error) => {
      setDraft(null)
      onError(e)
    },
  })
  const value = draft ?? d.mv
  const commit = (mv: number) => {
    if (Number.isFinite(mv)) {
      set.mutate(Math.round(Math.min(Math.max(mv, 0), VREF_MV)))
    }
  }

  return (
    <div className="flex items-center gap-3 text-sm">
      <ChannelLabel name={dacName(d)} pin={dacPin(d)} />
      <Slider
        className="min-w-0 flex-1"
        min={0}
        max={VREF_MV}
        step={10}
        value={[value]}
        disabled={set.isPending}
        onValueChange={(v) => setDraft(Array.isArray(v) ? v[0] : v)}
        onValueCommitted={(v) => commit(Array.isArray(v) ? v[0] : v)}
        aria-label={`${dacName(d)} (mV)`}
      />
      <Input
        type="number"
        className="w-20"
        min={0}
        max={VREF_MV}
        value={value}
        disabled={set.isPending}
        onChange={(e) => setDraft(e.target.valueAsNumber)}
        onBlur={() => draft !== null && commit(draft)}
        onKeyDown={(e) => {
          if (e.key === "Enter" && draft !== null) {
            commit(draft)
          }
        }}
        aria-label={`${dacName(d)} (mV)`}
      />
      <span className="text-muted-foreground">mV</span>
    </div>
  )
}

// The DAC channels that can be set: already enabled, or with their pin in
// analog mode (configured for the DAC in CubeMX); e.g. not DAC1 OUT2 when
// its pin PA5 is a GPIO output.
function Dacs({
  dacs,
  pins,
  error,
}: {
  dacs: DacOutput[]
  pins: Map<string, GpioPin>
  error?: string
}) {
  const usable = dacs.filter(
    (d) => d.enabled || pins.get(dacPin(d) ?? "")?.mode === "ANL"
  )
  return (
    <Card>
      <CardHeader>
        <CardTitle>DAC outputs</CardTitle>
      </CardHeader>
      <CardContent className="flex flex-col gap-3">
        {error && <p className="text-sm text-destructive">{error}</p>}
        {!error && usable.length === 0 && (
          <p className="text-sm text-muted-foreground">No DAC channel.</p>
        )}
        {usable.map((d) => (
          <DacRow key={dacKey(d)} d={d} />
        ))}
      </CardContent>
    </Card>
  )
}

export function IOs() {
  // the configuration does not change while the board runs: read once
  const list = useQuery({
    queryKey: ["adc list"],
    queryFn: adcList,
    staleTime: Infinity,
  })
  // timer-triggered ADCs (the scope) cannot be read one conversion at a time
  const adcs: AdcChannel[] = (list.data?.adcs ?? [])
    .filter((a) => a.trigger === "software")
    .flatMap((a) => a.channels.map((ch) => ({ n: a.adc, ch })))

  const ios = useQuery({
    queryKey: ["ios", adcs],
    queryFn: () => poll(adcs),
    refetchInterval: POLL_MS,
    // a new result at each poll, even with the same values: the graphs
    // scroll at each refresh
    structuralSharing: false,
  })
  const pins = ios.data?.pins ?? new Map<string, GpioPin>()

  return (
    <div className="grid items-start gap-4 lg:grid-cols-2">
      {ios.isError && (
        <p className="text-sm text-destructive lg:col-span-2">
          {ios.error.message}
        </p>
      )}
      <div className="flex flex-col gap-4">
        <GpioLevels pins={pins} />
        <AnalogLevels
          adcs={adcs}
          adcSamples={ios.data?.adcSamples ?? []}
          adcError={list.error?.message}
          dacs={ios.data?.dacs ?? []}
          dacError={ios.data?.dacError}
        />
      </div>
      <div className="flex flex-col gap-4">
        <GpioOutputs pins={pins} />
        <Dacs
          dacs={ios.data?.dacs ?? []}
          pins={pins}
          error={ios.data?.dacError}
        />
      </div>
    </div>
  )
}
