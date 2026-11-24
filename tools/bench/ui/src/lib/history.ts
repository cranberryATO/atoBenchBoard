// Recent samples of the polled signals (GPIO levels, ADC values), for the
// scrolling graphs. Kept outside React: written by the polling functions,
// read when rendering. Survives the navigation (the polling stops meanwhile).

export const HISTORY_LENGTH = 120 // samples per signal

const histories = new Map<string, number[]>()

/** Append a sample (NaN: no value, a gap in the graph). */
export function pushSample(key: string, value: number) {
  const h = histories.get(key) ?? []
  h.push(value)
  if (h.length > HISTORY_LENGTH) {
    h.shift()
  }
  histories.set(key, h)
}

export function samples(key: string): readonly number[] {
  return histories.get(key) ?? []
}
