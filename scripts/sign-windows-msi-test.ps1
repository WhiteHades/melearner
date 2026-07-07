param(
  [string]$MsiDirectory = "src-tauri\target\x86_64-pc-windows-gnu\release\bundle\msi",
  [Parameter(Mandatory = $true)][string]$PfxPath,
  [Parameter(Mandatory = $true)][string]$PfxPassword,
  [Parameter(Mandatory = $true)][string]$PublicCertPath,
  [string]$Subject = "CN=melearner Windows MSI test artifact"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-SignTool {
  $roots = @(
    [Environment]::GetFolderPath("ProgramFilesX86"),
    [Environment]::GetFolderPath("ProgramFiles")
  ) | Where-Object { $_ }

  $matches = @()
  foreach ($root in $roots) {
    $binRoot = Join-Path $root "Windows Kits\10\bin"
    if (!(Test-Path -LiteralPath $binRoot)) {
      continue
    }

    foreach ($versionDir in @(Get-ChildItem -LiteralPath $binRoot -Directory -ErrorAction SilentlyContinue)) {
      $candidate = Join-Path $versionDir.FullName "x64\signtool.exe"
      if (Test-Path -LiteralPath $candidate) {
        $matches += @(Get-Item -LiteralPath $candidate)
      }
    }
  }

  return $matches | Sort-Object FullName -Descending | Select-Object -First 1
}

function Invoke-Checked {
  param(
    [string]$Description,
    [string]$Command,
    [string[]]$Arguments
  )

  Write-Output $Description
  $previousErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $output = & $Command @Arguments 2>&1
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }
  if ($output) {
    $output | ForEach-Object { Write-Output $_ }
  }
  if ($exitCode -ne 0) {
    throw "$Description failed with exit code $exitCode"
  }
}

if (!(Test-Path -LiteralPath $MsiDirectory)) {
  throw "MSI directory does not exist: $MsiDirectory"
}
if (!(Test-Path -LiteralPath $PfxPath)) {
  throw "PFX file does not exist: $PfxPath"
}
if (!(Test-Path -LiteralPath $PublicCertPath)) {
  throw "public certificate file does not exist: $PublicCertPath"
}

Write-Output "finding Windows SDK signtool.exe"
$signTool = Find-SignTool
if (!$signTool) {
  throw "could not find Windows SDK signtool.exe; install the Windows SDK Signing Tools"
}
Write-Output "using signtool at $($signTool.FullName)"

$msiFiles = @(Get-ChildItem -LiteralPath $MsiDirectory -Filter "*.msi" -File)
if ($msiFiles.Count -lt 1) {
  throw "no MSI files found in $MsiDirectory"
}
Write-Output "found $($msiFiles.Count) MSI file(s) in $MsiDirectory"

Invoke-Checked `
  -Description "trusting public test certificate in CurrentUser Root store" `
  -Command "certutil.exe" `
  -Arguments @("-user", "-addstore", "Root", $PublicCertPath)

Invoke-Checked `
  -Description "trusting public test certificate in CurrentUser TrustedPublisher store" `
  -Command "certutil.exe" `
  -Arguments @("-user", "-addstore", "TrustedPublisher", $PublicCertPath)

foreach ($msi in $msiFiles) {
  Invoke-Checked `
    -Description "signing $($msi.FullName)" `
    -Command $signTool.FullName `
    -Arguments @("sign", "/f", $PfxPath, "/p", $PfxPassword, "/fd", "SHA256", "/v", $msi.FullName)

  Invoke-Checked `
    -Description "verifying $($msi.FullName)" `
    -Command $signTool.FullName `
    -Arguments @("verify", "/pa", "/v", $msi.FullName)

  Write-Output "signed $($msi.FullName) with $Subject"
}
