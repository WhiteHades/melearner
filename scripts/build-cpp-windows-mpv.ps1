param(
  [Parameter(Mandatory)] [string] $VcpkgInstalled,
  [Parameter(Mandatory)] [string] $OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceRevision = 'v0.41.0'
$sourceUrl = 'https://github.com/mpv-player/mpv/archive/refs/tags/v0.41.0.tar.gz'
$archiveName = 'mpv-0.41.0.tar.gz'
$expectedArchiveSha256 = 'ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209'
$libplaceboCommit = '1fd3c7bde7b943fe8985c893310b5269a09b46c5'
$libplaceboVersion = '7.349.0'
$libplaceboUrl = 'https://github.com/haasn/libplacebo.git'
$sdkRoot = [IO.Path]::GetFullPath($OutputDir)
$archivePath = Join-Path $sdkRoot $archiveName
$sourceRoot = Join-Path $sdkRoot 'source'
$buildRoot = Join-Path $sdkRoot 'build'
$noticeRoot = Join-Path $sdkRoot 'source-notices'
$libplaceboRoot = Join-Path $env:RUNNER_TEMP 'melearner-libplacebo-source'
$libplaceboBuildRoot = Join-Path $env:RUNNER_TEMP 'melearner-libplacebo-build'
$libplaceboArchiveName = 'libplacebo-source.tar.gz'
$libplaceboArchive = Join-Path $sdkRoot $libplaceboArchiveName
$pkgconf = Join-Path $VcpkgInstalled 'tools\pkgconf\pkgconf.exe'
$tar = (Get-Command tar.exe -ErrorAction Stop).Source

if (Test-Path $sdkRoot) { throw "Refusing to overwrite existing mpv SDK output: $sdkRoot" }
if ((Test-Path $libplaceboRoot) -or (Test-Path $libplaceboBuildRoot)) { throw 'Refusing to overwrite existing libplacebo source/build directories' }
New-Item -ItemType Directory -Force $sdkRoot | Out-Null
Invoke-WebRequest -Uri $sourceUrl -OutFile $archivePath
$actualArchiveSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualArchiveSha256 -ne $expectedArchiveSha256) { throw "mpv source archive SHA-256 mismatch: $actualArchiveSha256" }

$extractRoot = Join-Path $sdkRoot 'extract'
New-Item -ItemType Directory $extractRoot | Out-Null
& $tar -xf $archivePath -C $extractRoot
if ($LASTEXITCODE -ne 0) { throw 'Could not extract the pinned mpv source archive' }
$sourceDir = Get-ChildItem -LiteralPath $extractRoot -Directory | Select-Object -First 1
if (-not $sourceDir -or -not (Test-Path (Join-Path $sourceDir.FullName 'meson.build'))) { throw 'Pinned mpv archive has an unexpected layout' }
Move-Item -LiteralPath $sourceDir.FullName -Destination $sourceRoot
Remove-Item -LiteralPath $extractRoot -Recurse -Force
New-Item -ItemType Directory -Force $noticeRoot | Out-Null
foreach ($name in @('Copyright', 'LICENSE.GPL', 'LICENSE.LGPL')) {
  $license = Join-Path $sourceRoot $name
  if (-not (Test-Path -LiteralPath $license -PathType Leaf)) { throw "Pinned mpv source is missing $name" }
  Copy-Item -LiteralPath $license -Destination $noticeRoot
}

if (-not (Test-Path -LiteralPath $pkgconf -PathType Leaf)) { throw "pkgconf not found: $pkgconf" }
$env:PKG_CONFIG = $pkgconf
$env:PKG_CONFIG_PATH = "$VcpkgInstalled\lib\pkgconfig;$VcpkgInstalled\share\pkgconfig"
$env:PKG_CONFIG_LIBDIR = $env:PKG_CONFIG_PATH
$env:CC = 'clang'
$env:CXX = 'clang++'
$env:CFLAGS = '--target=x86_64-pc-windows-msvc'
$env:CXXFLAGS = '--target=x86_64-pc-windows-msvc'
$env:LDFLAGS = '--target=x86_64-pc-windows-msvc'
$libplaceboBuildOptions = @(
  '-Dauto_features=disabled', '-Ddefault_library=shared', '-Dopengl=enabled',
  '-Dgl-proc-addr=enabled', '-Dvulkan=disabled', '-Ddemos=false', '-Dtests=false'
)
& git clone --filter=blob:none --no-checkout $libplaceboUrl $libplaceboRoot
if ($LASTEXITCODE -ne 0) { throw 'Could not clone the libplacebo source repository' }
& git -C $libplaceboRoot checkout --detach $libplaceboCommit
if ($LASTEXITCODE -ne 0) { throw 'Could not check out the pinned libplacebo commit' }
$actualLibplaceboCommit = (& git -C $libplaceboRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualLibplaceboCommit -ne $libplaceboCommit) { throw 'libplacebo checkout did not match its pinned commit' }
& git -C $libplaceboRoot submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize pinned libplacebo submodules' }
$submoduleRecords = @()
$submoduleStatus = & git -C $libplaceboRoot submodule status --recursive
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect libplacebo submodule revisions' }
foreach ($line in $submoduleStatus) {
  if ($line -notmatch '^ ([0-9a-f]{40}) (.+?)(?: \(.*\))?$') { throw "libplacebo submodule is not at its recorded gitlink: $line" }
  $submoduleRecords += [ordered]@{ path=$Matches[2]; commit=$Matches[1] }
}
& $tar -czf $libplaceboArchive --exclude=.git -C $libplaceboRoot .
if ($LASTEXITCODE -ne 0) { throw 'Could not archive the pinned libplacebo source and submodules' }
$libplaceboNotices = Join-Path $noticeRoot 'libplacebo'
New-Item -ItemType Directory -Force $libplaceboNotices | Out-Null
$sourceFiles = Get-ChildItem -LiteralPath $libplaceboRoot -File -Recurse -Force | Where-Object {
  $_.FullName -notmatch '[\\/]\.git[\\/]' -and
    ($_.Name -match '(?i)^(license|copying|notice|copyright|authors)([._ -].*)?$' -or $_.FullName -match '[\\/]LICENSES[\\/]')
}
foreach ($file in $sourceFiles) {
  $relativePath = [IO.Path]::GetRelativePath($libplaceboRoot, $file.FullName)
  $destination = Join-Path $libplaceboNotices $relativePath
  New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $destination
}
$libplaceboNoticeFiles = @(Get-ChildItem -LiteralPath $libplaceboNotices -File -Recurse)
if (-not $libplaceboNoticeFiles) { throw 'No libplacebo or submodule copyright notices were collected' }
& meson setup $libplaceboBuildRoot $libplaceboRoot --prefix=$sdkRoot --libdir=lib --buildtype=release --wrap-mode=nofallback @libplaceboBuildOptions
if ($LASTEXITCODE -ne 0) { throw 'libplacebo Meson configuration failed' }
& meson compile -C $libplaceboBuildRoot --jobs 2
if ($LASTEXITCODE -ne 0) { throw 'libplacebo compilation failed' }
& meson install -C $libplaceboBuildRoot
if ($LASTEXITCODE -ne 0) { throw 'libplacebo installation failed' }

$dependencyMetadata = @([ordered]@{
  name = 'libplacebo'
  version = $libplaceboVersion
  sourceURL = $libplaceboUrl
  sourceHash = $libplaceboCommit
  gitCommit = $libplaceboCommit
  sourceArchive = $libplaceboArchiveName
  noticeRoot = 'source-notices/libplacebo'
  buildOptions = $libplaceboBuildOptions
  submodules = $submoduleRecords
})

$env:PKG_CONFIG = $pkgconf
$env:PKG_CONFIG_PATH = "$sdkRoot\lib\pkgconfig;$VcpkgInstalled\lib\pkgconfig;$VcpkgInstalled\share\pkgconfig"
$env:PKG_CONFIG_LIBDIR = $env:PKG_CONFIG_PATH
$buildOptions = @(
  '-Dauto_features=disabled', '-Dbuild-date=false',
  '-Ddefault_library=shared', '-Dgpl=true', '-Dcplayer=false', '-Dlibmpv=true',
  '-Dtests=false', '-Dfuzzers=false', '-Dlibavdevice=disabled', '-Dlibarchive=disabled',
  '-Dlibbluray=disabled', '-Dcdda=disabled', '-Ddvbin=disabled', '-Ddvdnav=disabled',
  '-Dvapoursynth=disabled', '-Dcplugins=disabled', '-Dlua=disabled', '-Djavascript=disabled',
  '-Dwasapi=enabled', '-Dwin32-threads=enabled', '-Dplain-gl=enabled', '-Dgl=enabled',
  '-Dgl-win32=enabled', '-Dd3d-hwaccel=enabled', '-Dd3d9-hwaccel=enabled',
  '-Dwin32-smtc=enabled', '-Dmanpage-build=disabled', '-Dhtml-build=disabled', '-Dpdf-build=disabled'
)
& meson setup $buildRoot $sourceRoot --prefix=$sdkRoot --libdir=lib --buildtype=release --wrap-mode=nofallback @buildOptions
if ($LASTEXITCODE -ne 0) { throw 'mpv Meson configuration failed' }
& meson compile -C $buildRoot --jobs 2
if ($LASTEXITCODE -ne 0) { throw 'mpv compilation failed' }
& meson install -C $buildRoot
if ($LASTEXITCODE -ne 0) { throw 'mpv installation failed' }

$dllCandidates = @(Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'bin') -Filter '*mpv*.dll' -File)
if ($dllCandidates.Count -ne 1) { throw "Expected one mpv DLL in the SDK bin directory; found $($dllCandidates.Count)" }
$dll = $dllCandidates[0].FullName
if ($dllCandidates[0].Name -ne 'libmpv-2.dll') {
  $canonicalDll = Join-Path $sdkRoot 'bin\libmpv-2.dll'
  Move-Item -LiteralPath $dll -Destination $canonicalDll
  $dll = $canonicalDll
}
$header = Join-Path $sdkRoot 'include\mpv\client.h'
if (-not (Test-Path -LiteralPath $header -PathType Leaf)) { throw 'mpv install did not provide include/mpv/client.h' }

$exports = & dumpbin /nologo /exports $dll
if ($LASTEXITCODE -ne 0) { throw 'dumpbin could not read libmpv exports' }
$names = foreach ($line in $exports) {
  if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] }
}
if (-not $names) { throw 'libmpv export list was empty' }
$libDir = Join-Path $sdkRoot 'lib'
New-Item -ItemType Directory -Force $libDir | Out-Null
$defFile = Join-Path $libDir 'libmpv.def'
@('LIBRARY libmpv-2.dll', 'EXPORTS') + $names | Set-Content -Encoding ascii $defFile
$importLibrary = Join-Path $libDir 'mpv.lib'
& lib.exe /nologo "/def:$defFile" /machine:x64 "/out:$importLibrary"
if ($LASTEXITCODE -ne 0) { throw 'MSVC mpv import library generation failed' }

$pcDir = Join-Path $libDir 'pkgconfig'
New-Item -ItemType Directory -Force $pcDir | Out-Null
$prefix = $sdkRoot.Replace('\', '/')
@"
prefix=$prefix
exec_prefix=`${prefix}
libdir=`${prefix}/lib
includedir=`${prefix}/include

Name: mpv
Description: libmpv 0.41.0 source-built Windows SDK
Version: 2.5.0
Libs: -L`${libdir} -lmpv
Cflags: -I`${includedir}
"@ | Set-Content -Encoding ascii (Join-Path $pcDir 'mpv.pc')

$manifest = [ordered]@{
  name = 'mpv'
  version = '0.41.0'
  sourceURL = $sourceUrl
  sourceSHA256 = $actualArchiveSha256
  buildOptions = $buildOptions
  sourceArchive = $archiveName
  noticeRoot = 'source-notices'
  dependencies = $dependencyMetadata
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $sdkRoot 'source-manifest.json')
Write-Host "Built libmpv from $sourceRevision into $sdkRoot"
"MPV_SDK=$sdkRoot" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"MPV_DLL=$dll" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"MPV_SOURCE_ARCHIVE=$archivePath" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
Remove-Item -LiteralPath $libplaceboRoot -Recurse -Force
Remove-Item -LiteralPath $libplaceboBuildRoot -Recurse -Force
