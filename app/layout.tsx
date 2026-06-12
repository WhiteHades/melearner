import type { Metadata } from "next"
import { AppProviders } from "@/components/app-providers"
import { EvlogClientProvider } from "@/components/evlog-client-provider"
import { Atkinson_Hyperlegible, Geist_Mono } from "next/font/google"
import { TitleBar } from "@/components/title-bar"
import "@/app/globals.css"

const atkinson = Atkinson_Hyperlegible({
  subsets: ["latin"],
  weight: ["400", "700"],
  variable: "--font-sans",
  display: "swap",
})

const geistMono = Geist_Mono({
  subsets: ["latin"],
  variable: "--font-mono",
  display: "swap",
})

export const metadata: Metadata = {
  title: "melearn",
  description: "A simple offline course learner for local libraries.",
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode
}) {
  return (
    <html lang="en" suppressHydrationWarning>
      <body className={`${atkinson.variable} ${geistMono.variable} flex h-screen min-h-screen flex-col overflow-hidden bg-background font-sans text-foreground antialiased`}>
        <EvlogClientProvider>
          <AppProviders>
            <TitleBar />
            <div className="min-h-0 flex-1 w-full overflow-hidden">
              {children}
            </div>
          </AppProviders>
        </EvlogClientProvider>
      </body>
    </html>
  )
}
