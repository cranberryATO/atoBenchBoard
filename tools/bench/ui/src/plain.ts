// Text rendering of a JSON reply, equivalent to the plain mode of the CLI:
//   flat reply:   "ok key=value key=value" / "error: message"
//   nested data:  generic rendering, close to the plain tables of the
//                 firmware: arrays of flat objects as aligned tables,
//                 arrays of values 16 per line, objects indented.
// Numbers lose their format (20000.000 is shown as 20000).

import type { Reply } from "@/cli"

type Json = null | boolean | number | string | Json[] | { [key: string]: Json }
type JsonObject = { [key: string]: Json }

const VALUES_PER_LINE = 16

function isObject(v: Json): v is JsonObject {
  return typeof v === "object" && v !== null && !Array.isArray(v)
}

function isScalar(v: Json): boolean {
  return v === null || typeof v !== "object"
}

function scalar(v: Json): string {
  return v === null ? "null" : String(v)
}

/** "key=value" of the scalar members. */
function pairs(obj: JsonObject): string[] {
  return Object.entries(obj)
    .filter(([, v]) => isScalar(v))
    .map(([k, v]) => `${k}=${scalar(v)}`)
}

function table(rows: JsonObject[], indent: string): string[] {
  const columns = [...new Set(rows.flatMap((r) => Object.keys(r)))]
  const cells = rows.map((r) => columns.map((c) => (c in r ? scalar(r[c]) : "")))
  const widths = columns.map((c, i) =>
    Math.max(c.length, ...cells.map((row) => row[i].length))
  )
  const line = (row: string[]) =>
    indent + row.map((cell, i) => cell.padEnd(widths[i])).join("  ").trimEnd()
  return [line(columns), ...cells.map(line)]
}

function array(arr: Json[], indent: string): string[] {
  if (arr.length === 0) {
    return [`${indent}(none)`]
  }
  if (arr.every(isScalar)) {
    const lines = []
    for (let i = 0; i < arr.length; i += VALUES_PER_LINE) {
      lines.push(indent + arr.slice(i, i + VALUES_PER_LINE).map(scalar).join(" "))
    }
    return lines
  }
  if (arr.every((v) => isObject(v) && Object.values(v).every(isScalar))) {
    return table(arr as JsonObject[], indent)
  }
  // objects with nested data, or mixed content: one block per element
  return arr.flatMap((v) => (isObject(v) ? object(v, indent) : array([v], indent)))
}

/** The scalars on a first line (after `head`), then the nested members. */
function object(obj: JsonObject, indent: string, head = ""): string[] {
  const first = [head, ...pairs(obj)].filter((s) => s !== "").join(" ")
  const lines = first ? [indent + first] : []
  for (const [k, v] of Object.entries(obj)) {
    if (Array.isArray(v)) {
      lines.push(`${indent}${k}:`, ...array(v, indent + "  "))
    } else if (isObject(v)) {
      lines.push(`${indent}${k}:`, ...object(v, indent + "  "))
    }
  }
  return lines
}

export function toPlain(reply: Reply): string {
  if (reply.ok !== true) {
    return `error: ${String(reply.error ?? "invalid reply")}`
  }
  const rest = Object.fromEntries(
    Object.entries(reply).filter(([k]) => k !== "ok")
  ) as JsonObject
  return object(rest, "", "ok").join("\n")
}
