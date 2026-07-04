import { cn } from "@/lib/utils"

export function LogoMark({ className }: { className?: string }) {
  return (
    <svg
      viewBox="0 0 64 64"
      fill="none"
      className={cn("text-foreground", className)}
      aria-hidden="true"
    >
      <rect x="4.5" y="4.5" width="55" height="55" rx="14" fill="var(--card)" stroke="var(--border)" />
      <text
        x="11"
        y="30"
        fill="currentColor"
        fontFamily="Arial, Helvetica, sans-serif"
        fontSize="20"
        fontWeight="700"
      >
        me
      </text>
      <text
        x="11"
        y="45"
        fill="currentColor"
        fontFamily="Arial, Helvetica, sans-serif"
        fontSize="13"
        fontWeight="700"
      >
        learner
      </text>
    </svg>
  )
}

export function BrandLogo({ className }: { className?: string }) {
  return (
    <div className={cn("flex min-w-0 items-center", className)} aria-label="melearner">
      <LogoMark className="size-11 shrink-0" />
    </div>
  )
}
