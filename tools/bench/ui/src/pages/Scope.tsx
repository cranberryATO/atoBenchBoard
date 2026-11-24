import { useMutation } from "@tanstack/react-query"
import { useState } from "react"
import { toast } from "sonner"

import { acq, dump, gen, type Dump, type Shape } from "@/cli"
import { NumberField } from "@/components/NumberField"
import { ScopeChart } from "@/components/ScopeChart"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { Label } from "@/components/ui/label"
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select"

export function Scope() {
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
      toast.success(
        `${d.n} points at ${a.fs} Hz` + (a.f ? `, f = ${a.f} Hz` : "")
      )
    },
    onError: (e: Error) => toast.error(e.message),
  })

  return (
    <Card>
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
          <NumberField
            id="gen-vpp"
            label="Vpp (mV)"
            value={vpp}
            onChange={setVpp}
          />
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
