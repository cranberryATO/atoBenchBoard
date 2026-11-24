// useState that survives the unmount of the page (navigation) and a reload
// of the tab: kept in memory and in sessionStorage. If the storage is not
// available or full (large replies), the memory copy still covers the
// navigation within the app.

import { useCallback, useState } from "react"

const memory = new Map<string, unknown>()

function load<T>(key: string, initial: T): T {
  if (memory.has(key)) {
    return memory.get(key) as T
  }
  try {
    const stored = sessionStorage.getItem(key)
    if (stored !== null) {
      return JSON.parse(stored) as T
    }
  } catch {
    // storage unavailable or invalid content: start from `initial`
  }
  return initial
}

function save(key: string, value: unknown) {
  memory.set(key, value)
  try {
    sessionStorage.setItem(key, JSON.stringify(value))
  } catch {
    // quota exceeded or storage unavailable: the memory copy remains
  }
}

export function useSessionState<T>(
  key: string,
  initial: T
): [T, (update: T | ((previous: T) => T)) => void] {
  const [value, setValue] = useState<T>(() => load(key, initial))
  const set = useCallback(
    (update: T | ((previous: T) => T)) =>
      setValue((previous) => {
        const next =
          typeof update === "function"
            ? (update as (previous: T) => T)(previous)
            : update
        save(key, next)
        return next
      }),
    [key]
  )
  return [value, set]
}
