// Package view (QFP, 4 sides), like the pinout view of STM32CubeMX: each
// pad is colored by the actual mode of its pin, with its function outside.

import type { GpioPin } from "@/cli"
import pinout from "@/data/pinout.json"

export type PackagePin = (typeof pinout.pins)[number]

export type PadState = "IN" | "OUT" | "AF" | "ANL" | "power" | "off"

export const PAD_STATES: { state: PadState; label: string }[] = [
  { state: "IN", label: "Input" },
  { state: "OUT", label: "Output" },
  { state: "AF", label: "Alternate function" },
  { state: "ANL", label: "Analog" },
  { state: "power", label: "Power / reference" },
  { state: "off", label: "Unknown (port clock off)" },
]

export function padState(pin: PackagePin, gpio?: GpioPin): PadState {
  if (pin.gpio === null) {
    return "power"
  }
  return gpio?.mode ?? "off"
}

/** Text outside the pad: pin name, and its function if known. */
export function padLabel(pin: PackagePin, gpio?: GpioPin): string {
  if (gpio === undefined) {
    return pin.name
  }
  const func = gpio.mode === "AF" ? gpio.af : gpio.mode
  return `${pin.name} · ${func}`
}

const PER_SIDE = pinout.pins.length / 4
const PITCH = 22
const PAD_LEN = 22
const PAD_W = 13
const BODY = PER_SIDE * PITCH + PITCH // body side, a pitch of margin
const MARGIN = 190 // room for the labels
const B0 = MARGIN // body edges
const B1 = MARGIN + BODY
const SIZE = 2 * MARGIN + BODY
const FIRST = B0 + PITCH // center of the first pad of a side

type Geometry = {
  x: number // pad rectangle
  y: number
  w: number
  h: number
  tx: number // label anchor
  ty: number
  rotate: boolean // vertical label (top and bottom sides)
  anchor: "start" | "end"
}

// Pin 1 at the top of the left side, numbered counterclockwise.
function geometry(position: number): Geometry {
  const side = Math.floor((position - 1) / PER_SIDE)
  const i = (position - 1) % PER_SIDE
  switch (side) {
    case 0: {
      const y = FIRST + i * PITCH
      return {
        x: B0 - PAD_LEN, y: y - PAD_W / 2, w: PAD_LEN, h: PAD_W,
        tx: B0 - PAD_LEN - 6, ty: y, rotate: false, anchor: "end",
      }
    }
    case 1: {
      const x = FIRST + i * PITCH
      return {
        x: x - PAD_W / 2, y: B1, w: PAD_W, h: PAD_LEN,
        tx: x, ty: B1 + PAD_LEN + 6, rotate: true, anchor: "end",
      }
    }
    case 2: {
      const y = B1 - PITCH - i * PITCH
      return {
        x: B1, y: y - PAD_W / 2, w: PAD_LEN, h: PAD_W,
        tx: B1 + PAD_LEN + 6, ty: y, rotate: false, anchor: "start",
      }
    }
    default: {
      const x = B1 - PITCH - i * PITCH
      return {
        x: x - PAD_W / 2, y: B0 - PAD_LEN, w: PAD_W, h: PAD_LEN,
        tx: x, ty: B0 - PAD_LEN - 6, rotate: true, anchor: "start",
      }
    }
  }
}

const FILL: Record<PadState, string> = {
  IN: "var(--pin-in)",
  OUT: "var(--pin-out)",
  AF: "var(--pin-af)",
  ANL: "var(--pin-anl)",
  power: "var(--pin-power)",
  off: "var(--pin-off)",
}

type Props = {
  gpio: Map<string, GpioPin>
  selected: number | null
  onSelect: (position: number) => void
}

export function Lqfp({ gpio, selected, onSelect }: Props) {
  return (
    <svg
      viewBox={`0 0 ${SIZE} ${SIZE}`}
      className="h-auto w-full max-w-[760px] select-none"
      role="img"
      aria-label={`${pinout.mcu} ${pinout.package} pinout`}
    >
      <rect
        x={B0} y={B0} width={BODY} height={BODY} rx={6}
        className="fill-muted stroke-border"
        strokeWidth={2}
      />
      <circle cx={B0 + 18} cy={B0 + 18} r={6} className="fill-muted-foreground" />
      <text
        x={SIZE / 2} y={SIZE / 2 - 8} textAnchor="middle"
        className="fill-foreground text-[18px] font-medium"
      >
        {pinout.mcu}
      </text>
      <text
        x={SIZE / 2} y={SIZE / 2 + 16} textAnchor="middle"
        className="fill-muted-foreground text-[14px]"
      >
        {pinout.package}
      </text>

      {pinout.pins.map((pin) => {
        const g = geometry(pin.position)
        const live = pin.gpio ? gpio.get(pin.gpio) : undefined
        const state = padState(pin, live)
        const isSelected = selected === pin.position
        return (
          <g
            key={pin.position}
            onClick={() => onSelect(pin.position)}
            className="cursor-pointer"
          >
            <title>{`${pin.position}: ${padLabel(pin, live)}`}</title>
            <rect
              x={g.x} y={g.y} width={g.w} height={g.h} rx={2}
              fill={FILL[state]}
              className={isSelected ? "stroke-foreground" : "stroke-border"}
              strokeWidth={isSelected ? 2.5 : 1}
            />
            <text
              x={g.tx} y={g.ty}
              textAnchor={g.anchor}
              dominantBaseline="central"
              transform={g.rotate ? `rotate(-90 ${g.tx} ${g.ty})` : undefined}
              className={
                isSelected
                  ? "fill-foreground text-[11px] font-semibold"
                  : "fill-foreground text-[11px]"
              }
            >
              {padLabel(pin, live)}
            </text>
          </g>
        )
      })}
    </svg>
  )
}
