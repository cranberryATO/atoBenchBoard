import { useQuery } from "@tanstack/react-query"

import { gpioShow, pinsByName } from "@/cli"
import { Lqfp, PAD_STATES, padState } from "@/components/Lqfp"
import { Badge } from "@/components/ui/badge"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import pinout from "@/data/pinout.json"
import { useSessionState } from "@/lib/session-state"

export function Pinout() {
  const gpio = useQuery({
    queryKey: ["gpio"],
    queryFn: gpioShow,
    refetchInterval: 2000,
  })
  // kept when leaving the page, and on a reload of the tab
  const [selected, setSelected] = useSessionState<number | null>(
    "pinout.selected",
    null
  )

  const pins = gpio.data ? pinsByName(gpio.data) : new Map()
  const pin = pinout.pins.find((p) => p.position === selected)
  const live = pin?.gpio ? pins.get(pin.gpio) : undefined

  return (
    <div className="flex flex-col gap-4 lg:flex-row lg:items-start">
      <Card className="flex-1">
        <CardContent className="flex justify-center">
          <Lqfp gpio={pins} selected={selected} onSelect={setSelected} />
        </CardContent>
      </Card>

      <div className="flex flex-col gap-4 lg:w-72">
        <Card>
          <CardHeader>
            <CardTitle>
              {pin ? `Pin ${pin.position}` : "No pin selected"}
            </CardTitle>
            <CardDescription>{pin?.name ?? "Click a pad"}</CardDescription>
          </CardHeader>
          {pin && (
            <CardContent>
              <dl className="grid grid-cols-2 gap-x-4 gap-y-1 text-sm">
                <dt className="text-muted-foreground">Type</dt>
                <dd>{pin.type}</dd>
                {live && (
                  <>
                    <dt className="text-muted-foreground">Mode</dt>
                    <dd>{live.mode}</dd>
                    <dt className="text-muted-foreground">Pull</dt>
                    <dd>{live.pull}</dd>
                    <dt className="text-muted-foreground">Level</dt>
                    <dd>{live.level}</dd>
                    <dt className="text-muted-foreground">Function</dt>
                    <dd className="break-all">{live.af}</dd>
                  </>
                )}
                {pin.gpio && !live && (
                  <dd className="col-span-2 text-muted-foreground">
                    No live data (port clock off, or board not connected)
                  </dd>
                )}
              </dl>
            </CardContent>
          )}
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Legend</CardTitle>
          </CardHeader>
          <CardContent className="flex flex-col gap-2 text-sm">
            {PAD_STATES.map(({ state, label }) => (
              <div key={state} className="flex items-center gap-2">
                <span
                  className="size-3 border"
                  style={{ background: `var(--pin-${state.toLowerCase()})` }}
                />
                {label}
                <Badge variant="outline" className="ml-auto">
                  {
                    pinout.pins.filter(
                      (p) =>
                        padState(p, p.gpio ? pins.get(p.gpio) : undefined) ===
                        state
                    ).length
                  }
                </Badge>
              </div>
            ))}
          </CardContent>
        </Card>

        {gpio.isError && (
          <Card>
            <CardContent className="text-sm text-destructive">
              {gpio.error.message}
            </CardContent>
          </Card>
        )}
      </div>
    </div>
  )
}
