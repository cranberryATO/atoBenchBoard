import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"
import { toast } from "sonner"

import { adcRead, dac, gpioSet, gpioShow, pinsByName, type GpioPin } from "@/cli"
import { NumberField } from "@/components/NumberField"
import { Sparkline } from "@/components/Sparkline"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { Label } from "@/components/ui/label"
import { Switch } from "@/components/ui/switch"
import { HISTORY_LENGTH, pushSample, samples } from "@/lib/history"
import { useLocalState } from "@/lib/session-state"

const POLL_MS = 250
const VREF_MV = 3300 // full scale of the ADC graphs

const onError = (e: Error) => toast.error(e.message)

type AdcChannel = { n: number; ch: number }
type AdcSample = AdcChannel & { mv?: number; error?: string }

const gpioKey = (pin: string) => `gpio:${pin}`
const adcKey = (a: AdcChannel) => `adc:${a.n}:${a.ch}`
const adcName = (a: AdcChannel) => `ADC${a.n} IN${a.ch}`

/*
 * One poll: the GPIO configuration and levels, then each ADC channel; the
 * samples of the inputs are appended to their history (scrolling graphs).
 */
async function poll(adcs: AdcChannel[]) {
  const pins = pinsByName(await gpioShow())
  for (const [name, p] of pins) {
    if (p.mode === "IN") {
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
  return { pins, adcSamples }
}

function pinsInMode(pins: Map<string, GpioPin>, mode: GpioPin["mode"]) {
  return [...pins].filter(([, p]) => p.mode === mode)
}

const seconds = (HISTORY_LENGTH * POLL_MS) / 1000

function GpioInputs({ pins }: { pins: Map<string, GpioPin> }) {
  const inputs = pinsInMode(pins, "IN")
  return (
    <Card>
      <CardHeader>
        <CardTitle>GPIO inputs</CardTitle>
        <CardDescription>
          Pins in input mode, the last {seconds} s.
        </CardDescription>
      </CardHeader>
      <CardContent className="flex flex-col gap-1.5">
        {inputs.length === 0 && (
          <p className="text-sm text-muted-foreground">No pin in input mode.</p>
        )}
        {inputs.map(([name, p]) => (
          <div key={name} className="flex items-center gap-3 text-sm">
            <span className="w-12 shrink-0">{name}</span>
            <Sparkline
              values={samples(gpioKey(name))}
              min={0}
              max={1}
              step
              color="var(--pin-in)"
              title={`${name}: ${p.level}`}
            />
          </div>
        ))}
      </CardContent>
    </Card>
  )
}

function AdcInputs({
  adcs,
  setAdcs,
  adcSamples,
}: {
  adcs: AdcChannel[]
  setAdcs: (update: (previous: AdcChannel[]) => AdcChannel[]) => void
  adcSamples: AdcSample[]
}) {
  const [n, setN] = useState(3)
  const [ch, setCh] = useState(1)
  const add = () =>
    setAdcs((list) =>
      list.some((a) => a.n === n && a.ch === ch) ? list : [...list, { n, ch }]
    )
  const remove = (a: AdcChannel) =>
    setAdcs((list) => list.filter((b) => b.n !== a.n || b.ch !== a.ch))

  return (
    <Card>
      <CardHeader>
        <CardTitle>ADC inputs</CardTitle>
        <CardDescription>
          Software-triggered ADC channels (adc), 0 to {VREF_MV} mV, the last{" "}
          {seconds} s.
        </CardDescription>
      </CardHeader>
      <CardContent className="flex flex-col gap-1.5">
        {adcs.length === 0 && (
          <p className="text-sm text-muted-foreground">No ADC channel.</p>
        )}
        {adcs.map((a) => {
          const s = adcSamples.find((x) => x.n === a.n && x.ch === a.ch)
          return (
            <div key={adcKey(a)} className="flex items-center gap-3 text-sm">
              <span className="w-20 shrink-0">{adcName(a)}</span>
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
                  title={s?.mv !== undefined ? `${adcName(a)}: ${s.mv} mV` : undefined}
                />
              )}
              <Button
                variant="ghost"
                size="icon-xs"
                aria-label={`Remove ${adcName(a)}`}
                onClick={() => remove(a)}
              >
                ×
              </Button>
            </div>
          )
        })}
        <div className="mt-3 grid grid-cols-3 items-end gap-3">
          <NumberField id="adc-n" label="ADC" value={n} onChange={setN} />
          <NumberField id="adc-ch" label="Channel" value={ch} onChange={setCh} />
          <Button variant="outline" onClick={add}>
            Add
          </Button>
        </div>
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
        <CardDescription>Pins configured as outputs in CubeMX.</CardDescription>
      </CardHeader>
      <CardContent className="flex flex-col gap-3">
        {outputs.length === 0 && (
          <p className="text-sm text-muted-foreground">No pin in output mode.</p>
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

function Dac() {
  const [n, setN] = useState(1)
  const [ch, setCh] = useState(1)
  const [mv, setMv] = useState(1000)
  const set = useMutation({
    mutationFn: () => dac(n, ch, mv),
    onSuccess: (r) =>
      toast.success(`DAC${n} channel ${ch}: ${r.mv} mV (raw ${r.raw})`),
    onError,
  })
  return (
    <Card>
      <CardHeader>
        <CardTitle>DAC</CardTitle>
        <CardDescription>Static output voltage.</CardDescription>
      </CardHeader>
      <CardContent className="grid grid-cols-3 items-end gap-3">
        <NumberField id="dac-n" label="DAC" value={n} onChange={setN} />
        <NumberField id="dac-ch" label="Channel" value={ch} onChange={setCh} />
        <NumberField id="dac-mv" label="mV" value={mv} onChange={setMv} />
        <Button
          className="col-span-3"
          disabled={set.isPending}
          onClick={() => set.mutate()}
        >
          Set
        </Button>
      </CardContent>
    </Card>
  )
}

export function IOs() {
  // ADC3 IN1: the software-triggered channel configured in CubeMX
  const [adcs, setAdcs] = useLocalState<AdcChannel[]>("ios.adcs", [
    { n: 3, ch: 1 },
  ])
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
        <GpioInputs pins={pins} />
        <AdcInputs
          adcs={adcs}
          setAdcs={setAdcs}
          adcSamples={ios.data?.adcSamples ?? []}
        />
      </div>
      <div className="flex flex-col gap-4">
        <GpioOutputs pins={pins} />
        <Dac />
      </div>
    </div>
  )
}
