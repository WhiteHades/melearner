param(
  [string]$MsiDirectory = "src-tauri\target\x86_64-pc-windows-gnu\release\bundle\msi",
  [Parameter(Mandatory = $true)][string]$PfxPath,
  [Parameter(Mandatory = $true)][string]$PfxPassword,
  [string]$PublicCertPath = "",
  [string]$Subject = "Windows code-signing certificate",
  [string]$TimestampUrl = "",
  [switch]$AllowUntrustedRoot
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

function Invoke-Native {
  param(
    [string]$Description,
    [string]$Command,
    [string[]]$Arguments
  )

  Write-Host $Description
  $previousErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $output = & $Command @Arguments 2>&1
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }
  if ($output) {
    $output | ForEach-Object { Write-Host $_ }
  }

  return [pscustomobject]@{
    ExitCode = $exitCode
    Output = $output
  }
}

function Invoke-Checked {
  param(
    [string]$Description,
    [string]$Command,
    [string[]]$Arguments
  )

  $result = Invoke-Native -Description $Description -Command $Command -Arguments $Arguments
  $exitCode = $result.ExitCode
  if ($exitCode -ne 0) {
    throw "$Description failed with exit code $exitCode"
  }
}

function Test-SignedFile {
  param(
    [string]$SignToolPath,
    [string]$Path,
    [switch]$AllowUntrustedRoot
  )

  $result = Invoke-Native `
    -Description "checking Authenticode signature on $Path" `
    -Command $SignToolPath `
    -Arguments @("verify", "/pa", "/v", $Path)

  if ($result.ExitCode -eq 0) {
    return
  }

  $verificationOutput = ($result.Output | Out-String)
  if ($verificationOutput -match "No signature found" -or $verificationOutput -match "not valid") {
    throw "signtool did not find an Authenticode signature on $Path"
  }
  if ($verificationOutput -match "not trusted" -or $verificationOutput -match "root certificate") {
    if ($AllowUntrustedRoot) {
      Write-Output "signature is present; self-signed test certificate is expected to be untrusted"
      $global:LASTEXITCODE = 0
      return
    }

    throw "signature chain is not trusted for $Path"
  }

  throw "unexpected signtool verification failure for $Path"
}

if (!(Test-Path -LiteralPath $MsiDirectory)) {
  throw "MSI directory does not exist: $MsiDirectory"
}
if (!(Test-Path -LiteralPath $PfxPath)) {
  throw "PFX file does not exist: $PfxPath"
}
if ($PublicCertPath -and !(Test-Path -LiteralPath $PublicCertPath)) {
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

foreach ($msi in $msiFiles) {
  $signArguments = @("sign", "/f", $PfxPath, "/p", $PfxPassword, "/fd", "SHA256")
  if ($TimestampUrl) {
    $signArguments += @("/tr", $TimestampUrl, "/td", "SHA256")
  }
  $signArguments += @("/v", $msi.FullName)

  Invoke-Checked `
    -Description "signing $($msi.FullName)" `
    -Command $signTool.FullName `
    -Arguments $signArguments

  Test-SignedFile -SignToolPath $signTool.FullName -Path $msi.FullName -AllowUntrustedRoot:$AllowUntrustedRoot

  Write-Output "signed $($msi.FullName) with $Subject"
}
