"use client"

import { NuqsAdapter } from "nuqs/adapters/next/app"
import { AppBootstrap } from "@/components/app-bootstrap"
import { ThemeProvider } from "next-themes"
import { TRPCProvider } from "@/components/trpc-provider"

export function AppProviders({ children }: { children: React.ReactNode }) {
  return (
    <NuqsAdapter>
      <ThemeProvider
        attribute="class"
        defaultTheme="light"
        enableSystem={false}
        themes={["light", "dark"]}
        disableTransitionOnChange
      >
        <TRPCProvider>
          <AppBootstrap />
          {children}
        </TRPCProvider>
      </ThemeProvider>
    </NuqsAdapter>
  )
}
