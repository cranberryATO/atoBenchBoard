// useState that survives the unmount of the page (navigation) and a reload:
// kept in memory and in sessionStorage (the tab) or localStorage (the
// browser, e.g. settings). If the storage is not available or full (large
// replies), the memory copy still covers the navigation within the app.

import { useCallback, useState } from "react"

type Scope = "session" | "local"

const memory = new Map<string, unknown>()

function storage(scope: Scope): Storage {
  return scope === "local" ? localStorage : sessionStorage
}

function load<T>(scope: Scope, key: string, initial: T): T {
  if (memory.has(key)) {
    return memory.get(key) as T
  }
  try {
    const stored = storage(scope).getItem(key)
    if (stored !== null) {
      return JSON.parse(stored) as T
    }
  } catch {
    // storage unavailable or invalid content: start from `initial`
  }
  return initial
}

function save(scope: Scope, key: string, value: unknown) {
  memory.set(key, value)
  try {
    storage(scope).setItem(key, JSON.stringify(value))
  } catch {
    // quota exceeded or storage unavailable: the memory copy remains
  }
}

function useStoredState<T>(
  scope: Scope,
  key: string,
  initial: T
): [T, (update: T | ((previous: T) => T)) => void] {
  const [value, setValue] = useState<T>(() => load(scope, key, initial))
  const set = useCallback(
    (update: T | ((previous: T) => T)) =>
      setValue((previous) => {
        const next =
          typeof update === "function"
            ? (update as (previous: T) => T)(previous)
            : update
        save(scope, key, next)
        return next
      }),
    [scope, key]
  )
  return [value, set]
}

/** Kept for the tab (sessionStorage). */
export function useSessionState<T>(key: string, initial: T) {
  return useStoredState("session", key, initial)
}

/** Kept for the browser (localStorage): settings. */
export function useLocalState<T>(key: string, initial: T) {
  return useStoredState("local", key, initial)
}
