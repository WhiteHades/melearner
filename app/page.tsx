import { Suspense } from "react"
import { HomeScreen } from "@/components/home-screen"

function HomeFallback() {
  return <main className="h-full min-h-0 bg-background" />
}

export default function Home() {
  return (
    <Suspense fallback={<HomeFallback />}>
      <main className="h-full min-h-0">
        <HomeScreen />
      </main>
    </Suspense>
  )
}
