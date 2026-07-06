"use client"

import { NuqsAdapter } from "nuqs/adapters/next/app"
import { Loader2 } from "lucide-react"
import { AppBootstrap } from "@/components/app-bootstrap"
import { TooltipProvider } from "@/components/ui/tooltip"
import { ThemeProvider } from "next-themes"
import { useCourseStore } from "@/lib/stores/course-store"

export function AppProviders({ children }: { children: React.ReactNode }) {
  const hasHydrated = useCourseStore((state) => state.hasHydrated)

  return (
    <ThemeProvider
      attribute="class"
      defaultTheme="cozy"
      enableSystem={false}
      themes={["light", "dark", "cozy"]}
      storageKey="melearner-theme"
      disableTransitionOnChange
    >
      <TooltipProvider>
        <AppBootstrap />
        {hasHydrated ? (
          <NuqsAdapter>
            {children}
          </NuqsAdapter>
        ) : (
          <main className="flex h-screen items-center justify-center bg-background text-foreground">
            <div className="flex items-center gap-2 text-sm text-muted-foreground">
              <Loader2 className="size-4 animate-spin" />
              Loading library
            </div>
          </main>
        )}
      </TooltipProvider>
    </ThemeProvider>
  )
}
