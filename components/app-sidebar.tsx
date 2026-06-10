"use client"

import React, { useState } from "react"
import { useCourseStore } from "@/lib/stores/course-store"
import { selectFolderDialog, isTauri } from "@/lib/tauri"
import { BookOpen, FolderOpen, Search, Settings, Moon, Sun, Loader2, RefreshCw } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert"
import { trpc } from "@/lib/trpc/client"
import { useTheme } from "next-themes"
import {
  Sidebar,
  SidebarContent,
  SidebarFooter,
  SidebarGroup,
  SidebarGroupContent,
  SidebarHeader,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
  SidebarRail,
  SidebarTrigger,
  SidebarSeparator,
  useSidebar,
} from "@/components/ui/sidebar"
import {
  CommandDialog,
  CommandEmpty,
  CommandGroup,
  CommandInput,
  CommandItem,
  CommandList,
} from "@/components/ui/command"

const navItems = [
  { title: "Library", icon: BookOpen, view: "library" as const },
  { title: "Search", icon: Search, view: "search" as const },
  { title: "Settings", icon: Settings, view: "settings" as const },
]

export function AppSidebar({ ...props }: React.ComponentProps<typeof Sidebar>) {
  const isScanning = useCourseStore((state) => state.isScanning)
  const libraryPath = useCourseStore((state) => state.libraryPath)
  const setIsScanning = useCourseStore((state) => state.setIsScanning)
  const [error, setError] = useState<string | null>(null)
  const [cmdOpen, setCmdOpen] = useState(false)
  const [activeView, setActiveView] = useState<string>("library")
  const { setOpen } = useSidebar()
  const { resolvedTheme, setTheme } = useTheme()
  const isDark = resolvedTheme === "dark"
  const utils = trpc.useUtils()
  const { data: courses = [] } = trpc.courses.list.useQuery()
  const scanLibrary = trpc.library.scan.useMutation({
    onSuccess: async () => {
      await utils.courses.list.invalidate()
    },
  })

  async function handleSelectFolder() {
    if (!isTauri()) {
      setError("Folder selection is only available in the desktop app.")
      return
    }

    try {
      const path = await selectFolderDialog()
      if (!path) return

      setIsScanning(true)
      setError(null)
      await scanLibrary.mutateAsync({ path })
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to scan the selected folder.")
    } finally {
      setIsScanning(false)
    }
  }

  async function handleRefresh() {
    if (!libraryPath) return

    try {
      setIsScanning(true)
      setError(null)
      await scanLibrary.mutateAsync({ path: libraryPath })
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to refresh the current library.")
    } finally {
      setIsScanning(false)
    }
  }

  function handleNavClick(item: typeof navItems[number]) {
    setActiveView(item.view)
    if (item.view === "search") {
      setCmdOpen(true)
    }
  }

  return (
    <>
      <Sidebar collapsible="offcanvas" {...props}>
        <SidebarHeader>
          <SidebarMenu>
            <SidebarMenuItem>
              <SidebarTrigger />
            </SidebarMenuItem>
          </SidebarMenu>
        </SidebarHeader>

        <SidebarContent>
          <SidebarGroup>
            <SidebarGroupContent>
              <SidebarMenu>
                {navItems.map((item) => (
                  <SidebarMenuItem key={item.title}>
                    <SidebarMenuButton
                      isActive={activeView === item.view}
                      tooltip={item.title}
                      onClick={() => handleNavClick(item)}
                    >
                      <item.icon />
                      <span>{item.title}</span>
                    </SidebarMenuButton>
                  </SidebarMenuItem>
                ))}
              </SidebarMenu>
            </SidebarGroupContent>
          </SidebarGroup>

          <SidebarSeparator />

          <SidebarGroup>
            <SidebarGroupContent>
              <SidebarMenu>
                <SidebarMenuItem>
                  <SidebarMenuButton onClick={() => setTheme(isDark ? "light" : "dark")} tooltip={isDark ? "Light mode" : "Dark mode"}>
                    {isDark ? <Sun className="size-4" /> : <Moon className="size-4" />}
                    <span>{isDark ? "Light mode" : "Dark mode"}</span>
                  </SidebarMenuButton>
                </SidebarMenuItem>
              </SidebarMenu>
            </SidebarGroupContent>
          </SidebarGroup>
        </SidebarContent>

        <SidebarFooter>
          <div className="space-y-2 p-2">
            {error && (
              <Alert variant="destructive" className="border-destructive/30 text-sm py-2">
                <AlertTitle className="text-xs">Error</AlertTitle>
                <AlertDescription className="text-xs">{error}</AlertDescription>
              </Alert>
            )}
            <div className="flex flex-col gap-1">
              <SidebarMenu>
                <SidebarMenuItem>
                  <SidebarMenuButton onClick={handleSelectFolder} disabled={isScanning} tooltip={libraryPath ? "Change folder" : "Choose folder"}>
                    <FolderOpen className="size-4" />
                    <span>{libraryPath ? "Change folder" : "Choose folder"}</span>
                    {isScanning && <Loader2 className="ml-auto size-3 animate-spin" />}
                  </SidebarMenuButton>
                </SidebarMenuItem>
                {libraryPath && (
                  <SidebarMenuItem>
                    <SidebarMenuButton onClick={handleRefresh} disabled={isScanning} tooltip="Refresh library">
                      <RefreshCw className={`size-4 ${isScanning ? "animate-spin" : ""}`} />
                      <span>Refresh</span>
                    </SidebarMenuButton>
                  </SidebarMenuItem>
                )}
              </SidebarMenu>
            </div>
            {libraryPath && (
              <p className="text-[10px] text-muted-foreground truncate px-2">{libraryPath}</p>
            )}
          </div>
        </SidebarFooter>
        <SidebarRail />
      </Sidebar>

      <CommandDialog open={cmdOpen} onOpenChange={setCmdOpen}>
        <CommandInput placeholder="Search courses, lessons, sections\u2026" />
        <CommandList>
          <CommandEmpty>No results found.</CommandEmpty>
          {courses.length > 0 && (
            <CommandGroup heading="Courses">
              {courses.map((course) => (
                <CommandItem
                  key={course.id}
                  value={`course:${course.id}:${course.name}`}
                  onSelect={() => {
                    setCmdOpen(false)
                    window.location.search = `?view=viewer&course=${course.id}`
                  }}
                >
                  <BookOpen className="size-4" />
                  <span>{course.name}</span>
                </CommandItem>
              ))}
            </CommandGroup>
          )}
        </CommandList>
      </CommandDialog>
    </>
  )
}