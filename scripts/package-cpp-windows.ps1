param(
  [string]$BuildDir = 'build/manual-windows',
  [string]$QtRoot = $env:MELEARNER_QT_ROOT,
  [string]$VcpkgBin = 'D:\v\installed\x64-windows-release\bin',
  [string]$MpvBin = $(if ($env:MPV_DLL) { Split-Path -Parent $env:MPV_DLL } elseif ($env:MPV_SDK) { $env:MPV_SDK }),
  [string]$LegalRoot = 'packaging',
  [string]$Output,
  [string]$Version
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Resolve-RepoPath([string]$Path) {
  if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
  return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Require-File([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
    throw "Required $Label is missing or empty: $Path"
  }
}

$buildPath = Resolve-RepoPath $BuildDir
$qtPath = if ($QtRoot) { [IO.Path]::GetFullPath($QtRoot) } else { throw 'Specify -QtRoot or set MELEARNER_QT_ROOT.' }
$vcpkgPath = [IO.Path]::GetFullPath($VcpkgBin)
$mpvPath = if ($MpvBin) { [IO.Path]::GetFullPath($MpvBin) } else { throw 'Specify -MpvBin or set MPV_SDK.' }
$legalPath = Resolve-RepoPath $LegalRoot
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
$appExe = Join-Path $buildPath 'melearner.exe'
Require-File $cachePath 'configured CMake cache'
Require-File $appExe 'Release application executable'

$cache = (Get-Content -LiteralPath $cachePath -Raw).Replace("`r`n", "`n")
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release$') { throw 'Build directory must be configured with CMAKE_BUILD_TYPE=Release.' }
$configuredSource = [regex]::Match($cache, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$').Groups[1].Value.Replace('/', '\').TrimEnd('\')
if (-not $configuredSource -or -not [string]::Equals($configuredSource, $repoRoot.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
  throw 'Build directory was not configured from this source tree.'
}
if ($cache -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=x64$' -and $cache -notmatch '(?m)^CMAKE_GENERATOR:INTERNAL=Ninja$') {
  throw 'Build directory must use the x64 Ninja generator or an explicitly x64 generator platform.'
}
$projectFile = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($projectFile -notmatch 'project\(melearner VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)') { throw 'Could not read melearner project version from CMakeLists.txt.' }
$projectVersion = $Matches[1]
if (-not $Version) { $Version = $projectVersion }
if ($Version -notmatch '^\d+\.\d+\.\d+$' -or $Version -ne $projectVersion) {
  throw "Installer version '$Version' must match the project version '$projectVersion'."
}
if (-not $Output) { $Output = "dist/melearner-$Version-setup.exe" }
$outputPath = Resolve-RepoPath $Output

$qtPaths = Join-Path $qtPath 'bin\qtpaths.exe'
$windeployqt = Join-Path $qtPath 'bin\windeployqt.exe'
Require-File $qtPaths 'Qt qtpaths.exe'
Require-File $windeployqt 'Qt windeployqt.exe'
$qtVersion = (& $qtPaths --query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -ne '6.11.2') { throw "Expected Qt 6.11.2, found '$qtVersion'." }
$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if (-not $iscc) { throw 'Inno Setup 6 ISCC.exe is required on PATH.' }
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) { throw 'Run from an MSVC developer shell; dumpbin.exe is required to resolve runtime DLLs.' }
$headers = & $dumpbin.Source /nologo /headers $appExe
if ($LASTEXITCODE -ne 0 -or -not ($headers -match '(?m)^\s*8664 machine \(x64\)\s*$')) {
  throw 'melearner.exe must be an x64 PE executable.'
}
foreach ($path in @($vcpkgPath, $mpvPath)) {
  if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Runtime DLL directory is missing: $path" }
}
foreach ($name in @('THIRD_PARTY_NOTICES', 'melearner.spdx.json', 'runtime-lock.json', 'reference-profiles-v1.json', 'QtWebEngine-LICENSE.chromium')) {
  Require-File (Join-Path $legalPath $name) "legal input $name"
}
Require-File (Join-Path $repoRoot 'LICENSE') 'project LICENSE'

if (Test-Path -LiteralPath $outputPath) { throw "Refusing to overwrite existing installer: $outputPath" }
$outputDir = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$stage = Join-Path ([IO.Path]::GetTempPath()) ("melearner-windows-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
  Copy-Item -LiteralPath $appExe -Destination (Join-Path $stage 'melearner.exe')
  & $windeployqt --release --no-compiler-runtime --webenginewidgets (Join-Path $stage 'melearner.exe')
  if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed while staging the application.' }

  # Release windeployqt copies vc_redist.exe, not app-local CRT libraries.
  # Use the active MSVC toolchain's redistributable DLLs for a per-user install
  # that requires neither elevation nor a separate runtime setup program.
  if (-not $env:VCToolsRedistDir) { throw 'The MSVC developer shell did not provide VCToolsRedistDir.' }
  $crtRoots = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT')
  if ($crtRoots.Count -ne 1) { throw 'Expected one x64 MSVC redistributable CRT directory in the active toolchain.' }
  $crtDLLs = @(Get-ChildItem -LiteralPath $crtRoots[0].FullName -File -Filter '*.dll')
  if (-not $crtDLLs.Count) { throw 'The active MSVC redistributable CRT directory contains no DLLs.' }
  $crtDLLs | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $stage }

  foreach ($name in @('libmpv-2.dll', 'vulkan-1.dll')) {
    $source = if ($name -eq 'libmpv-2.dll') { Join-Path $mpvPath $name } else { Join-Path $vcpkgPath $name }
    Require-File $source $name
    Copy-Item -LiteralPath $source -Destination $stage
  }
  foreach ($relative in @(
      'platforms\qwindows.dll', 'Qt6WebEngineCore.dll', 'QtWebEngineProcess.exe',
      'resources\icudtl.dat', 'resources\qtwebengine_resources.pak',
      'resources\qtwebengine_resources_100p.pak', 'resources\qtwebengine_resources_200p.pak')) {
    Require-File (Join-Path $stage $relative) "Qt runtime resource $relative"
  }
  $locales = Join-Path $stage 'translations\qtwebengine_locales'
  if (-not (Test-Path -LiteralPath $locales -PathType Container) -or -not (Get-ChildItem $locales -Filter '*.pak' -File)) {
    throw 'windeployqt did not stage Qt WebEngine locale resources.'
  }
  foreach ($name in @('vcruntime140.dll', 'msvcp140.dll')) {
    Require-File (Join-Path $stage $name) "locally deployed MSVC runtime $name"
  }

  # Copy the non-Qt runtime closure from the build SDKs. Iterate newly copied DLLs
  # too, since media and mpv DLLs have their own runtime dependencies.
  $system32 = Join-Path $env:SystemRoot 'System32'
  $searchRoots = @($stage, (Join-Path $qtPath 'bin'), $vcpkgPath, $mpvPath, $system32)
  $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  $pending = [Collections.Generic.Queue[string]]::new()
  Get-ChildItem $stage -Filter '*.dll' -File -Recurse | ForEach-Object { $pending.Enqueue($_.FullName) }
  Get-ChildItem $stage -Filter '*.exe' -File -Recurse | ForEach-Object { $pending.Enqueue($_.FullName) }
  while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if (-not $seen.Add($binary)) { continue }
    $imports = & $dumpbin.Source /nologo /dependents $binary
    if ($LASTEXITCODE -ne 0) { throw "dumpbin could not inspect runtime imports: $binary" }
    foreach ($line in $imports) {
      if ($line -notmatch '^\s+([\w.-]+\.dll)\s*$') { continue }
      $dependency = $Matches[1]
      if ($dependency -match '^(api|ext)-ms-') { continue }
      $resolved = $null
      foreach ($root in $searchRoots) {
        $candidate = Get-ChildItem -LiteralPath $root -Filter $dependency -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) { $resolved = $candidate.FullName; break }
      }
      if (-not $resolved) { throw "Unresolved runtime DLL: $($binary | Split-Path -Leaf) -> $dependency" }
      if ([string]::Equals((Split-Path $resolved -Parent), $system32, [StringComparison]::OrdinalIgnoreCase)) { continue }
      if ((Split-Path $resolved -Parent) -ne $stage) {
        $destination = Join-Path $stage $dependency
        if (-not (Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $resolved -Destination $destination }
        $pending.Enqueue($destination)
      }
    }
  }

  $legalStage = Join-Path $stage 'legal'
  New-Item -ItemType Directory -Path $legalStage | Out-Null
  Copy-Item (Join-Path $repoRoot 'LICENSE') $legalStage
  foreach ($name in @('THIRD_PARTY_NOTICES', 'melearner.spdx.json', 'runtime-lock.json', 'reference-profiles-v1.json')) {
    Copy-Item (Join-Path $legalPath $name) $legalStage
  }
  Copy-Item (Join-Path $legalPath 'QtWebEngine-LICENSE.chromium') $legalStage
  $defines = @("/DAppVersion=$Version", "/DStageDir=$stage", "/DOutputDir=$outputDir", "/DOutputBaseName=$([IO.Path]::GetFileNameWithoutExtension($outputPath))")
  & $iscc.Source @defines (Join-Path $repoRoot 'packaging\windows\melearner.iss')
  if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
  Require-File $outputPath 'built installer'
  Write-Host "Built unsigned installer: $outputPath"
  Write-Host 'Per-user install; no updater or runtime network download. No code signing was requested or performed.'
} finally {
  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
