param(
  [string]$BuildDir = 'build/manual-windows',
  [string]$VcpkgRoot = 'D:\v',
  [string]$MpvSdk = 'D:\v\mpv-sdk',
  [string]$MpvArchive = 'D:\v\mpv-dev.7z',
  [string]$QtRoot = $env:MELEARNER_QT_ROOT,
  [string]$OutputDir = '.tmp/manual-ci/windows-legal'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
function RepoPath([string]$Path) {
  if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
  return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
function Require-File([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
    throw "Required $Label is missing or empty: $Path"
  }
}
function Add-Missing([string]$Package, [string]$Reason) {
  $script:missing.Add([pscustomobject]@{ package = $Package; reason = $Reason })
}
function Add-Package([string]$Name, [string]$Version, [string]$Provider, [string]$Url, [string]$Hash, [string]$HashType = 'sha256', [string]$Status = 'not-applicable') {
  $script:packages.Add([pscustomobject]@{
    name = $Name; version = $Version; provider = $Provider; sourceURL = $Url
    sourceHash = $Hash; sourceHashType = $HashType; installationStatus = $Status
    licenseDeclared = 'NOASSERTION'; licenseConcluded = 'NOASSERTION'; copyrightText = 'NOASSERTION'
  })
}
function Add-Notice([string]$Package, [string]$Label, [string]$Path, [string]$SourceUrl) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
  $item = Get-Item -LiteralPath $Path
  if ($item.Length -eq 0) { return $false }
  $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
  $safe = $Package -replace '[^A-Za-z0-9_.+-]', '_'
  $relative = $hash.Substring(0, 16) + '-' + ($item.Name -replace '[^A-Za-z0-9_.+-]', '_')
  $evidencePath = Join-Path $script:evidenceRoot (Join-Path $safe $relative)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $evidencePath) | Out-Null
  Copy-Item -LiteralPath $Path -Destination $evidencePath
  $script:evidence.Add([pscustomobject]@{ package = $Package; label = $Label; path = [IO.Path]::GetRelativePath($script:outputRoot, $evidencePath).Replace('\', '/'); sha256 = $hash; sourceURL = $SourceUrl })
  $text = [IO.File]::ReadAllText($Path)
  if ($text.IndexOf([char]0) -ge 0) { Add-Missing $Package "Binary-looking notice skipped: $Path"; return $true }
  [void]$script:notices.AppendLine("`n`n--- $Package ($Label): $Path ---`n")
  [void]$script:notices.AppendLine($text)
  return $true
}

$buildRoot = RepoPath $BuildDir
$vcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot)
$installedRoot = Join-Path $vcpkgRoot 'installed\x64-windows-release'
$mpvRoot = [IO.Path]::GetFullPath($MpvSdk)
$mpvArchivePath = [IO.Path]::GetFullPath($MpvArchive)
$qtRoot = if ($QtRoot) { [IO.Path]::GetFullPath($QtRoot) } else { throw 'MELEARNER_QT_ROOT or -QtRoot is required.' }
$outputRoot = RepoPath $OutputDir
foreach ($path in @((Join-Path $buildRoot '_deps\lexbor-src'), (Join-Path $buildRoot '_deps\shadcn_cpp-src'), (Join-Path $mpvRoot 'bin'), $installedRoot, $qtRoot)) {
  if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Required build/runtime input directory is missing: $path" }
}
Require-File (Join-Path $repoRoot 'LICENSE') 'project LICENSE'
Require-File (Join-Path $vcpkgRoot 'versions\baseline.json') 'pinned vcpkg checkout metadata'
Require-File $mpvArchivePath 'pinned mpv SDK archive'
if (Test-Path -LiteralPath $outputRoot) { throw "Refusing to overwrite existing notice directory: $outputRoot" }
$tar = Get-Command tar.exe -ErrorAction Stop
$scratch = Join-Path $env:RUNNER_TEMP ("wn-" + [guid]::NewGuid().ToString('N').Substring(0, 10))
New-Item -ItemType Directory -Path $scratch, $outputRoot | Out-Null
$script:sourceRoot = $scratch
$script:outputRoot = $outputRoot
$script:evidenceRoot = Join-Path $outputRoot 'notice-evidence'
New-Item -ItemType Directory -Path $script:evidenceRoot | Out-Null
$script:packages = [Collections.Generic.List[object]]::new()
$script:evidence = [Collections.Generic.List[object]]::new()
$script:missing = [Collections.Generic.List[object]]::new()
$script:notices = [Text.StringBuilder]::new()
try {
  Copy-Item (Join-Path $repoRoot 'LICENSE') (Join-Path $outputRoot 'LICENSE')
  $thirdPartyUrl = 'https://github.com/WhiteHades/melearner'
  $projectVersionMatch = [regex]::Match((Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw), 'project\(melearner VERSION ([0-9.]+)')
  if (-not $projectVersionMatch.Success) { throw 'Could not read melearner version from CMakeLists.txt.' }
  $projectCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
  if ($LASTEXITCODE -ne 0 -or $projectCommit -notmatch '^[0-9a-f]{40}$') { throw 'Could not identify the melearner source revision.' }
  Add-Package 'melearner' $projectVersionMatch.Groups[1].Value 'source-tree' "$thirdPartyUrl/tree/$projectCommit" $projectCommit 'gitCommit'

  $vcpkgCommit = (& git -C $vcpkgRoot rev-parse HEAD).Trim()
  if ($LASTEXITCODE -ne 0 -or $vcpkgCommit -notmatch '^[0-9a-f]{40}$') { throw 'Could not identify pinned vcpkg source revision.' }
  $statusFile = Join-Path $vcpkgRoot 'installed\vcpkg\status'
  Require-File $statusFile 'vcpkg installed package status database'
  $statusText = Get-Content -LiteralPath $statusFile -Raw
  $installedPackages = @{}
  foreach ($paragraph in ($statusText -split '(?:\r?\n){2,}')) {
    if ($paragraph -notmatch '(?m)^Status: install ok installed\r?$') { continue }
    $name = [regex]::Match($paragraph, '(?m)^Package: (.+)$').Groups[1].Value.Trim()
    $version = [regex]::Match($paragraph, '(?m)^Version: (.+)$').Groups[1].Value.Trim()
    $status = [regex]::Match($paragraph, '(?m)^Status: (.+)$').Groups[1].Value.Trim()
    if ($name -and $version -and -not $installedPackages.ContainsKey($name)) {
      $installedPackages[$name] = [pscustomobject]@{ Version=$version; Status=$status }
    }
  }
  foreach ($name in ($installedPackages.Keys | Sort-Object)) {
    $portUrl = "https://github.com/microsoft/vcpkg/tree/$vcpkgCommit/ports/$name"
    Add-Package $name $installedPackages[$name].Version 'vcpkg-installed-status' $portUrl $vcpkgCommit 'pinnedVcpkgPortRevision' $installedPackages[$name].Status
    $copyright = Join-Path $installedRoot "share\$name\copyright"
    if (-not (Add-Notice $name 'installed-copyright' $copyright $portUrl)) { Add-Missing $name 'No non-empty vcpkg installed share/<package>/copyright file.' }
  }

  $lexborUrl = 'https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz'
  Add-Package 'lexbor' '3.0.0' 'cmake-fetchcontent' $lexborUrl 'eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2'
  foreach ($file in @('LICENSE', 'NOTICE')) {
    if (-not (Add-Notice 'lexbor' 'upstream-notice' (Join-Path $buildRoot "_deps\lexbor-src\$file") $lexborUrl)) { Add-Missing 'lexbor' "Missing upstream $file." }
  }
  $shadcnHash = 'a9e2555ee036ef42c02156dc04d5d2e837e53b9fad5f2c23523da8a0f4549992'
  $shadcnCommit = '5cc52a0edfc27a70c1d4e4b8e5586f21fba4860d'
  $shadcnUrl = "https://codeload.github.com/WhiteHades/shadcn-cpp/tar.gz/$shadcnCommit"
  Add-Package 'shadcn-cpp' $shadcnCommit 'cmake-fetchcontent' $shadcnUrl $shadcnHash
  foreach ($file in @('LICENSE', 'LICENSES/shadcn-MIT.txt', 'LICENSES/ui-components-MIT.txt', 'LICENSES/Geist-OFL.txt')) {
    if (-not (Add-Notice 'shadcn-cpp' 'upstream-notice' (Join-Path $buildRoot "_deps\shadcn_cpp-src\$file") $shadcnUrl)) { Add-Missing 'shadcn-cpp' "Missing upstream $file." }
  }

  $archiveSpecs = @(
    @{ Name='qtwebengine'; Module='qtwebengine'; Version='6.11.2'; Hash='6101c1aa00ff933d1b65ee5d167f76e8d71b9ac5b378b0111277723ebda7c163'; License='LICENSE.Chromium' },
    @{ Name='qtbase'; Module='qtbase'; Version='6.11.2'; Hash='5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22'; License='' },
    @{ Name='qtdeclarative'; Module='qtdeclarative'; Version='6.11.2'; Hash='215b7b70517e380123eabc6b92243f3c47b6f016a91d126057dbe53551c6b430'; License='' },
    @{ Name='qtwebchannel'; Module='qtwebchannel'; Version='6.11.2'; Hash='feb3149758bda887f0c292656194812dd2c227595badc8fbdeb2fb2311c5575f'; License='' },
    @{ Name='qtpositioning'; Module='qtpositioning'; Version='6.11.2'; Hash='d8cf15ad43a3b1520adac64cec5900028525cf75f01a4c82a0002e7ac1ae7499'; License='' }
  )
  foreach ($spec in $archiveSpecs) {
    $url = "https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/$($spec.Module)-everywhere-src-6.11.2.tar.xz"
    $archive = Join-Path $scratch "$($spec.Module).tar.xz"
    Invoke-WebRequest -Uri $url -OutFile $archive
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $spec.Hash) { throw "Qt source archive SHA-256 mismatch for $($spec.Module): $actual" }
    Add-Package $spec.Name $spec.Version 'qt-official-source-archive' $url $actual
    $listing = & $tar.Source -tf $archive
    if ($LASTEXITCODE -ne 0) { throw "Could not list Qt source archive: $($spec.Module)" }
    $legalPaths = @($listing | Where-Object { $_ -match '(?i)(^|/)LICENSES/.+|(^|/)(license|copying|copyright|notice|readme)([._-][^/]*)?$' })
    if (-not $legalPaths) { Add-Missing $spec.Name 'No license/copyright/notice/README paths found in verified source archive.'; continue }
    $pathList = Join-Path $scratch "$($spec.Module)-legal-paths.txt"
    [IO.File]::WriteAllLines($pathList, [string[]]$legalPaths, [Text.UTF8Encoding]::new($false))
    $extract = Join-Path $scratch $spec.Module
    New-Item -ItemType Directory -Path $extract | Out-Null
    & $tar.Source -xf $archive -C $extract -T $pathList
    if ($LASTEXITCODE -ne 0) { throw "Could not extract legal files from $($spec.Module) archive." }
    $found = 0
    foreach ($path in $legalPaths) {
      $file = Join-Path $extract ($path.Replace('/', '\'))
      if (Add-Notice $spec.Name 'upstream-source-notice' $file $url) { $found++ }
    }
    if ($found -eq 0) { Add-Missing $spec.Name 'Legal paths were listed but no readable, non-empty notices were collected.' }
    if ($spec.License) {
      $chromium = Get-ChildItem -LiteralPath $extract -Filter $spec.License -File -Recurse | Select-Object -First 1
      if (-not $chromium) { Add-Missing $spec.Name 'Pinned Qt WebEngine source did not yield LICENSE.Chromium.' }
      else { Copy-Item -LiteralPath $chromium.FullName -Destination (Join-Path $outputRoot 'QtWebEngine-LICENSE.chromium') }
    }
  }

  Add-Package 'qt-sdk' '6.11.2' 'official-qt-sdk' 'https://download.qt.io/online/qtsdkrepository/' '' 'notice-files-only'
  $qtLicenseDir = Join-Path $qtRoot 'licenses'
  if (Test-Path -LiteralPath $qtLicenseDir -PathType Container) {
    $qtNotices = Get-ChildItem -LiteralPath $qtLicenseDir -File -Recurse
    foreach ($file in $qtNotices) { [void](Add-Notice 'qt-sdk' 'installed-sdk-notice' $file.FullName 'https://download.qt.io/online/qtsdkrepository/') }
  } else { Add-Missing 'qt-sdk' 'No licenses directory found in the installed Qt SDK.' }

  $mpvUrl = 'https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261003/mpv-dev-x86_64-20261003-git-3186d369f9.7z'
  $mpvSha = (Get-FileHash -LiteralPath $mpvArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
  $expectedMpvSha = 'b8c2d656c1584d5f08196fcee5c2d0783d1f76021897c4e0b3cc30f94c79522e'
  if ($mpvSha -ne $expectedMpvSha) { throw "Pinned mpv SDK SHA-256 mismatch: $mpvSha" }
  Add-Package 'libmpv-2' '2.5.0' 'mpv-dev-sdk' $mpvUrl '3186d369f9f090cd1363be0ac46a037824b702c6' 'gitCommit'
  ($packages | Where-Object name -EQ 'libmpv-2' | Select-Object -First 1) | Add-Member -NotePropertyName archiveSHA256 -NotePropertyValue $mpvSha -Force
  $mpvSourceUrl = 'https://github.com/mpv-player/mpv/archive/3186d369f9f090cd1363be0ac46a037824b702c6.tar.gz'
  $mpvSourceArchive = Join-Path $scratch 'mpv-source.tar.gz'
  Invoke-WebRequest -Uri $mpvSourceUrl -OutFile $mpvSourceArchive
  Add-Package 'mpv-source' '3186d369f9f090cd1363be0ac46a037824b702c6' 'mpv-upstream-source' $mpvSourceUrl '3186d369f9f090cd1363be0ac46a037824b702c6' 'gitCommit'
  $mpvListing = & $tar.Source -tf $mpvSourceArchive
  if ($LASTEXITCODE -ne 0) { throw 'Could not list pinned mpv source archive.' }
  $mpvPaths = @($mpvListing | Where-Object { $_ -match '(?i)(^|/)(copyright|license\.gpl|license\.lgpl)$' })
  if (-not $mpvPaths) { Add-Missing 'mpv-source' 'No upstream Copyright/GPL/LGPL notice paths found at the pinned mpv revision.' }
  else {
    $pathList = Join-Path $scratch 'mpv-legal-paths.txt'
    [IO.File]::WriteAllLines($pathList, [string[]]$mpvPaths, [Text.UTF8Encoding]::new($false))
    $mpvExtract = Join-Path $scratch 'mpv-source'
    New-Item -ItemType Directory -Path $mpvExtract | Out-Null
    & $tar.Source -xf $mpvSourceArchive -C $mpvExtract -T $pathList
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract legal files from pinned mpv source.' }
    foreach ($path in $mpvPaths) {
      $file = Join-Path $mpvExtract ($path.Replace('/', '\'))
      if (-not (Add-Notice 'mpv-source' 'upstream-source-notice' $file $mpvSourceUrl)) { Add-Missing 'mpv-source' "Could not read pinned source notice: $path" }
    }
  }
  $mpvSdkSource = Get-ChildItem -LiteralPath $mpvRoot -File -Recurse | Where-Object { $_.Name -in @('Copyright', 'LICENSE.GPL', 'LICENSE.LGPL') }
  foreach ($name in @('Copyright', 'LICENSE.GPL', 'LICENSE.LGPL')) {
    $file = $mpvSdkSource | Where-Object Name -EQ $name | Select-Object -First 1
    if ($file) { [void](Add-Notice 'libmpv-2' 'sdk-upstream-notice' $file.FullName $mpvUrl) }
  }
  Add-Missing 'libmpv-2' 'The supplied static libmpv SDK does not identify the source revisions for its transitive linked libraries.'

  $ffmpegCommit = '98e92563a3b60dbf6d370fd3491d7f896398e4c1'
  $ffmpegUrl = "https://github.com/FFmpeg/FFmpeg/archive/$ffmpegCommit.tar.gz"
  $ffmpegPackage = $packages | Where-Object name -EQ 'ffmpeg' | Select-Object -First 1
  if (-not $ffmpegPackage) { throw 'FFmpeg version is missing from vcpkg installed status.' }
  $ffmpegArchive = Join-Path $scratch 'ffmpeg.tar.gz'
  Invoke-WebRequest -Uri $ffmpegUrl -OutFile $ffmpegArchive
  Add-Package 'mpv-ffmpeg' $ffmpegCommit 'mpv-static-sdk-upstream-reference' $ffmpegUrl $ffmpegCommit 'gitCommit'
  $ffmpegListing = & $tar.Source -tf $ffmpegArchive
  if ($LASTEXITCODE -ne 0) { throw 'Could not list pinned FFmpeg source archive.' }
  $ffmpegPaths = @($ffmpegListing | Where-Object { $_ -match '(?i)(^|/)(LICENSE|COPYING|README)([._-][^/]*)?$' })
  if (-not $ffmpegPaths) { Add-Missing 'mpv-ffmpeg' 'No license/copyright/readme paths found in pinned source.' }
  else {
    $pathList = Join-Path $scratch 'ffmpeg-legal-paths.txt'
    [IO.File]::WriteAllLines($pathList, [string[]]$ffmpegPaths, [Text.UTF8Encoding]::new($false))
    $extract = Join-Path $scratch 'ffmpeg'
    New-Item -ItemType Directory -Path $extract | Out-Null
    & $tar.Source -xf $ffmpegArchive -C $extract -T $pathList
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract legal files from pinned FFmpeg source.' }
    foreach ($path in $ffmpegPaths) {
      $file = Join-Path $extract ($path.Replace('/', '\'))
      if (-not (Add-Notice 'mpv-ffmpeg' 'upstream-source-notice' $file $ffmpegUrl)) { Add-Missing 'mpv-ffmpeg' "Could not read pinned source notice: $path" }
    }
  }

  if (-not (Test-Path (Join-Path $outputRoot 'QtWebEngine-LICENSE.chromium'))) { throw 'Required QtWebEngine-LICENSE.chromium was not collected.' }
  $dllRecords = [Collections.Generic.List[object]]::new()
  foreach ($source in @(
      @{ Kind='Qt'; Root=(Join-Path $qtRoot 'bin') },
      @{ Kind='vcpkg'; Root=(Join-Path $installedRoot 'bin') },
      @{ Kind='mpv-sdk'; Root=(Join-Path $mpvRoot 'bin') })) {
    Get-ChildItem -LiteralPath $source.Root -Filter '*.dll' -File -Recurse | ForEach-Object {
      $dllRecords.Add([pscustomobject]@{ provider=$source.Kind; path=[IO.Path]::GetRelativePath($source.Root, $_.FullName).Replace('\', '/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() })
    }
  }

  $noticePath = Join-Path $outputRoot 'THIRD_PARTY_NOTICES'
  [IO.File]::WriteAllText($noticePath, $script:notices.ToString(), [Text.UTF8Encoding]::new($false))
  $packageArray = $packages.ToArray()
  $evidenceArray = $evidence.ToArray()
  $missingArray = $missing.ToArray()
  $dllArray = $dllRecords.ToArray()
  $packagesPath = Join-Path $outputRoot 'runtime-lock.json'
  ([ordered]@{ schemaVersion=1; packages=$packageArray }) | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $packagesPath -Encoding utf8
  $namespace = "https://github.com/WhiteHades/melearner/spdx/windows/$($env:GITHUB_RUN_ID)-$([guid]::NewGuid().ToString('N'))"
  $spdxPackages = @($packageArray | ForEach-Object {
    [ordered]@{ SPDXID=('SPDXRef-' + ($_.name -replace '[^A-Za-z0-9.-]', '-')); name=$_.name; versionInfo=$_.version; downloadLocation=if ($_.sourceURL) { $_.sourceURL } else { 'NOASSERTION' }; filesAnalyzed=$false; licenseConcluded='NOASSERTION'; licenseDeclared='NOASSERTION'; copyrightText='NOASSERTION' }
  })
  $spdxFiles = @($evidenceArray | ForEach-Object -Begin { $index = 0 } -Process {
    $index++
    [ordered]@{ fileName="./$($_.path)"; SPDXID="SPDXRef-File-$index"; checksums=@(@{ algorithm='SHA256'; checksumValue=$_.sha256 }); licenseConcluded='NOASSERTION'; licenseInfoInFiles=@('NOASSERTION'); copyrightText='NOASSERTION' }
  })
  $spdx = [ordered]@{ spdxVersion='SPDX-2.3'; dataLicense='CC0-1.0'; SPDXID='SPDXRef-DOCUMENT'; name='melearner Windows diagnostic installer evidence'; documentNamespace=$namespace; creationInfo=@{ creators=@('Tool: collect-cpp-windows-notices.ps1'); created=(Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ") }; packages=$spdxPackages; files=$spdxFiles }
  $spdx | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $outputRoot 'melearner.spdx.json') -Encoding utf8
  $profiles = [ordered]@{ schemaVersion=1; releaseQualified=$false; evidenceStatus='observed-with-gaps'; legalApproval='not-provided'; packages=$packageArray; noticeFiles=$evidenceArray; installedRuntimeDLLs=$dllArray; missingEvidence=$missingArray; signing='unsigned' }
  $profiles | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $outputRoot 'reference-profiles-v1.json') -Encoding utf8
  Write-Host "Collected $($packages.Count) package records, $($evidence.Count) notice files, $($dllRecords.Count) DLL hashes, and $($missing.Count) evidence gaps. Release-qualified: false."
} finally {
  if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
