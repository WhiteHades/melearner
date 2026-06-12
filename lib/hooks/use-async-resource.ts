"use client"

import { useEffect, useState } from "react"

export type AsyncResourceState<T> =
  | { status: "loading" }
  | { status: "success"; data: T }
  | { status: "error"; error: string }

export function useAsyncResource<T>(
  loader: () => Promise<T>,
  deps: ReadonlyArray<unknown>,
  options?: { onSuccess?: (data: T) => void; onError?: (error: string) => void },
): AsyncResourceState<T> {
  const [state, setState] = useState<AsyncResourceState<T>>({ status: "loading" })

  useEffect(() => {
    let isActive = true
    setState({ status: "loading" })

    loader()
      .then((data) => {
        if (!isActive) return
        setState({ status: "success", data })
        options?.onSuccess?.(data)
      })
      .catch((err: unknown) => {
        if (!isActive) return
        const message = err instanceof Error ? err.message : String(err)
        setState({ status: "error", error: message })
        options?.onError?.(message)
      })

    return () => {
      isActive = false
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps)

  return state
}
