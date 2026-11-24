import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query"
import { useState } from "react"
import { toast } from "sonner"

import {
  acq,
  dac,
  dump,
  gen,
  gpioSet,
  gpioShow,
  pinsByName,
  type Dump,
  type GpioPin,
  type Shape,
} from "@/cli"
import { ScopeChart } from "@/components/ScopeChart"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select"
import { Switch } from "@/components/ui/switch"
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table"

const onError = (e: Error) => toast.error(e.message)

function NumberField({
  id,
  label,
  value,
  onChange,
}: {
  id: string
  label: string
  value: number
  onChange: (v: number) => void
}) {
  return (
    <div className="flex flex-col gap-1.5">
      <Label htmlFor={id}>{label}</Label>
      <Input
        id={id}
        type="number"
        value={value}
        onChange={(e) => onChange(e.target.valueAsNumber)}
      />
    </div>
  )
}

function pinsInMode(pins: Map<string, GpioPin>, mode: GpioPin["mode"]) {
  return [...pins].filter(([, p]) => p.mode === mode)
}

function GpioInputs({ pins }: { pins: Map<string, GpioPin> }) {
  const inputs = pinsInMode(pins, "IN")
  return (
    <Card>
      <CardHeader>
        <CardTitle>GPIO inputs</CardTitle>
        <CardDescription>Pins in input mode, refreshed every 0.5 s.</CardDescription>
      </CardHeader>
      <CardContent>
        {inputs.length === 0 ? (
          <p className="text-sm text-muted-foreground">No pin in input mode.</p>
        ) : (
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Pin</TableHead>
                <TableHead>Pull</TableHead>
                <TableHead>Level</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {inputs.map(([name, p]) => (
                <TableRow key={name}>
                  <TableCell>{name}</TableCell>
                  <TableCell>{p.pull}</TableCell>
                  <TableCell>
                    <Badge variant={p.level ? "default" : "outline"}>{p.level}</Badge>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </CardContent>
    </Card>
  )
}

function GpioOutputs({ pins }: { pins: Map<string, GpioPin> }) {
  const queryClient = useQueryClient()
  const set = useMutation({
    mutationFn: ({ pin, level }: { pin: string; level: 0 | 1 }) =>
      gpioSet(pin, level),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ["gpio"] }),
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
    onSuccess: (r) => toast.success(`DAC${n} channel ${ch}: ${r.mv} mV (raw ${r.raw})`),
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

function Scope() {
  const [shape, setShape] = useState<Shape>("step")
  const [f, setF] = useState(100)
  const [vpp, setVpp] = useState(2000)
  const [fs, setFs] = useState(20000)
  const [n, setN] = useState(2000)
  const [data, setData] = useState<Dump | null>(null)

  const run = useMutation({
    mutationFn: async () => {
      await gen(shape, f, vpp)
      const a = await acq(fs, n)
      return { a, d: await dump(n) }
    },
    onSuccess: ({ a, d }) => {
      setData(d)
      toast.success(`${d.n} points at ${a.fs} Hz` + (a.f ? `, f = ${a.f} Hz` : ""))
    },
    onError,
  })

  return (
    <Card className="lg:col-span-2">
      <CardHeader>
        <CardTitle>Generator and acquisition</CardTitle>
        <CardDescription>
          The DAC generates the waveform while the ADC samples the input and
          the output of the circuit (gen, acq, dump).
        </CardDescription>
      </CardHeader>
      <CardContent className="flex flex-col gap-4">
        <div className="grid grid-cols-2 items-end gap-3 sm:grid-cols-6">
          <div className="flex flex-col gap-1.5">
            <Label>Shape</Label>
            <Select value={shape} onValueChange={(v) => setShape(v as Shape)}>
              <SelectTrigger>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="step">step</SelectItem>
                <SelectItem value="sine">sine</SelectItem>
                <SelectItem value="square">square</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <NumberField id="gen-f" label="f (Hz)" value={f} onChange={setF} />
          <NumberField id="gen-vpp" label="Vpp (mV)" value={vpp} onChange={setVpp} />
          <NumberField id="acq-fs" label="fs (Hz)" value={fs} onChange={setFs} />
          <NumberField id="acq-n" label="Points" value={n} onChange={setN} />
          <Button disabled={run.isPending} onClick={() => run.mutate()}>
            {run.isPending ? "Acquiring…" : "Acquire"}
          </Button>
        </div>
        {data ? (
          <ScopeChart data={data} />
        ) : (
          <p className="text-sm text-muted-foreground">No acquisition yet.</p>
        )}
      </CardContent>
    </Card>
  )
}

export function IOs() {
  const gpio = useQuery({
    queryKey: ["gpio"],
    queryFn: gpioShow,
    refetchInterval: 500,
  })
  const pins = gpio.data ? pinsByName(gpio.data) : new Map<string, GpioPin>()
  return (
    <div className="grid gap-4 lg:grid-cols-2">
      {gpio.isError && (
        <p className="text-sm text-destructive lg:col-span-2">{gpio.error.message}</p>
      )}
      <GpioInputs pins={pins} />
      <div className="flex flex-col gap-4">
        <GpioOutputs pins={pins} />
        <Dac />
      </div>
      <Scope />
    </div>
  )
}
