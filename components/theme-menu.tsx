"use client"

import { useSyncExternalStore } from "react"
import { Check, Moon, Palette, Sun } from "lucide-react"
import { useTheme } from "next-themes"
import { Button } from "@/components/ui/button"
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu"
import { cn } from "@/lib/utils"

const THEME_OPTIONS = [
  { value: "light", label: "Light", icon: Sun },
  { value: "dark", label: "Dark", icon: Moon },
  { value: "cozy", label: "Cozy dark", icon: Palette },
] as const

function subscribeMounted() {
  return () => undefined
}

export function ThemeMenu({ className }: { className?: string }) {
  const { theme, setTheme } = useTheme()
  const mounted = useSyncExternalStore(subscribeMounted, () => true, () => false)
  const activeTheme = mounted && (theme === "light" || theme === "dark" || theme === "cozy") ? theme : "cozy"
  const ActiveIcon = THEME_OPTIONS.find((option) => option.value === activeTheme)?.icon ?? Palette

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button type="button" variant="ghost" size="icon" className={cn("size-9", className)} aria-label="Choose theme">
          <ActiveIcon className="size-4" />
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="w-44">
        {THEME_OPTIONS.map((option) => {
          const Icon = option.icon
          return (
            <DropdownMenuItem key={option.value} onClick={() => setTheme(option.value)}>
              <Icon className="size-4" />
              <span>{option.label}</span>
              {activeTheme === option.value && <Check className="ml-auto size-4" />}
            </DropdownMenuItem>
          )
        })}
      </DropdownMenuContent>
    </DropdownMenu>
  )
}
