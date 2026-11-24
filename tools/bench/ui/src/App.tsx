import { useQuery } from "@tanstack/react-query"
import { HashRouter, Navigate, NavLink, Route, Routes } from "react-router"

import { cli } from "@/cli"
import { Badge } from "@/components/ui/badge"
import { Toaster } from "@/components/ui/sonner"
import { Console } from "@/pages/Console"
import { IOs } from "@/pages/IOs"
import { Pinout } from "@/pages/Pinout"

const PAGES = [
  { path: "/pinout", label: "Pinout" },
  { path: "/ios", label: "IOs" },
  { path: "/console", label: "Console" },
]

/** Link with the board: "mode json" every 5 s, a harmless command. */
function Status() {
  const ping = useQuery({
    queryKey: ["ping"],
    queryFn: () => cli("mode json"),
    refetchInterval: 5000,
    retry: false,
  })
  if (ping.isPending) {
    return <Badge variant="outline">connecting…</Badge>
  }
  return ping.isError ? (
    <Badge variant="destructive" title={ping.error.message}>
      board not connected
    </Badge>
  ) : (
    <Badge>board connected</Badge>
  )
}

export function App() {
  // hash routes: the relay serves the built UI as static files, without
  // a fallback to index.html for the client routes
  return (
    <HashRouter>
      <div className="flex min-h-svh flex-col">
        <header className="flex items-center gap-6 border-b px-6 py-3">
          <h1 className="font-heading text-lg font-medium">atoBench</h1>
          <nav className="flex gap-1">
            {PAGES.map((p) => (
              <NavLink
                key={p.path}
                to={p.path}
                className={({ isActive }) =>
                  "rounded-md px-3 py-1.5 text-sm " +
                  (isActive
                    ? "bg-muted font-medium"
                    : "text-muted-foreground hover:text-foreground")
                }
              >
                {p.label}
              </NavLink>
            ))}
          </nav>
          <div className="ml-auto">
            <Status />
          </div>
        </header>
        <main className="flex-1 p-6">
          <Routes>
            <Route path="/" element={<Navigate to="/pinout" replace />} />
            <Route path="/pinout" element={<Pinout />} />
            <Route path="/ios" element={<IOs />} />
            <Route path="/console" element={<Console />} />
          </Routes>
        </main>
      </div>
      <Toaster />
    </HashRouter>
  )
}

export default App
