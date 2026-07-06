"use client"

import { NuqsAdapter } from "nuqs/adapters/next/app"
import { TooltipProvider } from "@/components/ui/tooltip"
import { ThemeProvider } from "next-themes"

export function AppProviders({ children }: { children: React.ReactNode }) {
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
        <NuqsAdapter>
          {children}
        </NuqsAdapter>
      </TooltipProvider>
    </ThemeProvider>
  )
}
