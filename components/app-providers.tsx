"use client"

import { NuqsAdapter } from "nuqs/adapters/next/app"
import { AppBootstrap } from "@/components/app-bootstrap"
import { ThemeProvider } from "next-themes"

export function AppProviders({ children }: { children: React.ReactNode }) {
  return (
    <NuqsAdapter>
        <ThemeProvider
          attribute="class"
          defaultTheme="cozy"
          enableSystem={false}
          themes={["light", "dark", "cozy"]}
          storageKey="melearner-theme"
          disableTransitionOnChange
        >
        <AppBootstrap />
        {children}
      </ThemeProvider>
    </NuqsAdapter>
  )
}
