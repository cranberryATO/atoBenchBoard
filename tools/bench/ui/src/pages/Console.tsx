import { useMutation } from "@tanstack/react-query"
import { useState } from "react"

import { cliRaw, type Reply } from "@/cli"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardAction,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { Switch } from "@/components/ui/switch"
import { useSessionState } from "@/lib/session-state"
import { toPlain } from "@/plain"

// the raw reply is kept: the display can be switched afterwards
type Entry = { id: number; line: string; reply?: Reply; failure?: string }

function render(e: Entry, json: boolean): string {
  if (e.reply === undefined) {
    return e.failure ?? ""
  }
  return json ? JSON.stringify(e.reply, null, 2) : toPlain(e.reply)
}

export function Console() {
  const [line, setLine] = useState("")
  // kept when leaving the page, and on a reload of the tab
  const [json, setJson] = useSessionState("console.json", false) // plain by default
  const [history, setHistory] = useSessionState<Entry[]>("console.history", [])

  const add = (e: Omit<Entry, "id">) =>
    setHistory((h) => [{ id: Date.now(), ...e }, ...h].slice(0, 50))

  const send = useMutation({
    mutationFn: (l: string) => cliRaw(l, { replyBytes: 50000, timeout: 10 }),
    onSuccess: (reply, l) => add({ line: l, reply }),
    onError: (e, l) => add({ line: l, failure: e.message }),
  })

  function submit(e: React.FormEvent) {
    e.preventDefault()
    const l = line.trim()
    if (l) {
      send.mutate(l)
      setLine("")
    }
  }

  return (
    <Card>
      <CardHeader>
        <CardTitle>Console</CardTitle>
        <CardDescription>
          Any CLI command line ("help" lists the commands). The board always
          answers in JSON; the plain view renders it as text.
        </CardDescription>
        <CardAction className="flex items-center gap-2">
          <Label htmlFor="console-json">JSON</Label>
          <Switch id="console-json" checked={json} onCheckedChange={setJson} />
        </CardAction>
      </CardHeader>
      <CardContent className="flex flex-col gap-4">
        <form onSubmit={submit} className="flex gap-2">
          <Input
            value={line}
            onChange={(e) => setLine(e.target.value)}
            placeholder="gpio show"
            className="font-mono"
            autoFocus
          />
          <Button type="submit" disabled={send.isPending}>
            Send
          </Button>
        </form>
        {history.map((e) => (
          <div key={e.id} className="flex flex-col gap-1">
            <div className="font-mono text-sm">&gt; {e.line}</div>
            <pre
              className={
                "overflow-x-auto bg-muted p-3 text-xs " +
                (e.reply?.ok === true ? "" : "text-destructive")
              }
            >
              {render(e, json)}
            </pre>
          </div>
        ))}
      </CardContent>
    </Card>
  )
}
