import { useMutation } from "@tanstack/react-query"
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
import { useSessionState } from "@/lib/session-state"
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select"

export function Scope() {
  // kept when leaving the page, and on a reload of the tab
  const [shape, setShape] = useSessionState<Shape>("scope.shape", "step")
  const [f, setF] = useSessionState("scope.f", 100)
  const [vpp, setVpp] = useSessionState("scope.vpp", 2000)
  const [fs, setFs] = useSessionState("scope.fs", 20000)
  const [n, setN] = useSessionState("scope.n", 2000)
  // last acquisition: up to 2 x 4096 values, a few tens of KB
  const [data, setData] = useSessionState<Dump | null>("scope.data", null)

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
          This function generates a weveform on <strong>DAC 1.1 (PA4)</strong>{" "}
          while <strong>ADC 1.1 (PA0)</strong> and <strong>1.5 (PB14)</strong>{" "}
          sample their input. Connect ADC 1.1 to the input of the circuit and
          ADC 1.5 to the output.
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
          <NumberField
            id="acq-fs"
            label="fs (Hz)"
            value={fs}
            onChange={setFs}
          />
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
