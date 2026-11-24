// Curves of an acquisition (dump): input and output of the circuit, in
// volts, versus time in ms. uPlot: fast with thousands of points.

import { useEffect, useRef } from "react"
import uPlot from "uplot"
import "uplot/dist/uPlot.min.css"

import type { Dump } from "@/cli"

function cssVar(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim()
}

function toSeries(d: Dump): uPlot.AlignedData {
  const toVolt = d.vref / d.full / 1000
  const t = d.in.map((_, i) => (i / d.fs) * 1000)
  return [t, d.in.map((v) => v * toVolt), d.out.map((v) => v * toVolt)]
}

export function ScopeChart({ data }: { data: Dump }) {
  const box = useRef<HTMLDivElement>(null)
  const plot = useRef<uPlot | null>(null)

  // created once per label set (and full scale), updated with setData
  useEffect(() => {
    const el = box.current
    if (el === null) {
      return
    }
    const axis = {
      stroke: cssVar("--muted-foreground"),
      grid: { stroke: cssVar("--border") },
      ticks: { stroke: cssVar("--border") },
    }
    const opts: uPlot.Options = {
      width: el.clientWidth,
      height: 320,
      scales: {
        x: { time: false },
        // the whole ADC range, whatever the values: no zoom on small signals
        y: { range: [0, data.vref / 1000] },
      },
      axes: [
        { ...axis, label: "time (ms)" },
        { ...axis, label: "voltage (V)" },
      ],
      series: [
        { label: "t (ms)" },
        { label: `input (${data.in_label})`, stroke: cssVar("--pin-in"), width: 2 },
        { label: `output (${data.out_label})`, stroke: cssVar("--pin-af"), width: 2 },
      ],
    }
    plot.current = new uPlot(opts, toSeries(data), el)
    const resize = new ResizeObserver(() =>
      plot.current?.setSize({ width: el.clientWidth, height: 320 })
    )
    resize.observe(el)
    return () => {
      resize.disconnect()
      plot.current?.destroy()
      plot.current = null
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [data.in_label, data.out_label, data.vref])

  useEffect(() => {
    plot.current?.setData(toSeries(data))
  }, [data])

  return <div ref={box} className="w-full" />
}
