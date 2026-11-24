// Small scrolling graph, as high as a text line: the newest sample at the
// right edge. Steps for digital signals, lines for analog ones; NaN samples
// are gaps.

import { HISTORY_LENGTH } from "@/lib/history"

type Props = {
  values: readonly number[]
  min: number
  max: number
  step?: boolean // hold each value until the next sample (digital)
  color: string // CSS color, e.g. "var(--pin-in)"
  title?: string
}

const H = 20 // drawing height, in user units
const PAD = 2

export function Sparkline({ values, min, max, step = false, color, title }: Props) {
  const x0 = HISTORY_LENGTH - values.length // right-aligned
  const y = (v: number) =>
    H - PAD - ((Math.min(Math.max(v, min), max) - min) / (max - min)) * (H - 2 * PAD)

  // continuous runs of valid samples
  const lines: string[] = []
  const areas: string[] = []
  let run: string[] = []
  let start = 0
  const flush = (end: number) => {
    if (run.length > 0) {
      const line = "M" + run.join("L")
      lines.push(line)
      areas.push(`${line}L${end},${H - PAD}L${start},${H - PAD}Z`)
    }
    run = []
  }
  values.forEach((v, i) => {
    const x = x0 + i
    if (Number.isNaN(v)) {
      flush(x)
      return
    }
    if (run.length === 0) {
      start = step ? x : x + 0.5
    }
    if (step) {
      run.push(`${x},${y(v)}`, `${x + 1},${y(v)}`)
    } else {
      run.push(`${x + 0.5},${y(v)}`)
    }
  })
  flush(step ? HISTORY_LENGTH : HISTORY_LENGTH - 0.5)

  return (
    <svg
      viewBox={`0 0 ${HISTORY_LENGTH} ${H}`}
      preserveAspectRatio="none"
      className="h-5 w-full min-w-0 flex-1 bg-muted"
      role="img"
    >
      {title && <title>{title}</title>}
      {areas.map((d, i) => (
        <path key={`a${i}`} d={d} fill={color} opacity={0.15} />
      ))}
      {lines.map((d, i) => (
        <path
          key={`l${i}`}
          d={d}
          fill="none"
          stroke={color}
          strokeWidth={1.5}
          vectorEffect="non-scaling-stroke"
        />
      ))}
    </svg>
  )
}
