"use client"

import { NuqsAdapter } from "nuqs/adapters/next/app"
import { AppBootstrap } from "@/components/app-bootstrap"
import { ThemeProvider } from "next-themes"

export function AppProviders({ children }: { children: React.ReactNode }) {
  return (
    <NuqsAdapter>
      <ThemeProvider
        attribute="class"
        defaultTheme="dark"
        enableSystem={false}
        themes={["light", "dark"]}
        disableTransitionOnChange
      >
        <AppBootstrap />
        {children}
      </ThemeProvider>
    </NuqsAdapter>
  )
}
