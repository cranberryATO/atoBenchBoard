import { useMutation } from "@tanstack/react-query"
import { useState } from "react"

import { cliRaw } from "@/cli"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { Input } from "@/components/ui/input"

type Entry = { id: number; line: string; ok: boolean; text: string }

export function Console() {
  const [line, setLine] = useState("")
  const [history, setHistory] = useState<Entry[]>([])

  const send = useMutation({
    mutationFn: (l: string) => cliRaw(l, { replyBytes: 50000, timeout: 10 }),
    onSuccess: (reply, l) =>
      add(l, reply.ok === true, JSON.stringify(reply, null, 2)),
    onError: (e, l) => add(l, false, e.message),
  })

  function add(l: string, ok: boolean, text: string) {
    setHistory((h) => [{ id: Date.now(), line: l, ok, text }, ...h].slice(0, 50))
  }

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
          Any CLI command line, and the raw JSON reply ("help" lists the
          commands).
        </CardDescription>
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
                "max-h-80 overflow-auto rounded-md bg-muted p-3 text-xs " +
                (e.ok ? "" : "text-destructive")
              }
            >
              {e.text}
            </pre>
          </div>
        ))}
      </CardContent>
    </Card>
  )
}
