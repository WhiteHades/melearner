param(
  [string]$MsiDirectory = "src-tauri\target\x86_64-pc-windows-gnu\release\bundle\msi",
  [string]$Subject = "CN=melearner Windows MSI test artifact",
  [int]$ValidDays = 14
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

function Find-OpenSsl {
  $candidates = @(
    (Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "Git\usr\bin\openssl.exe"),
    (Join-Path ([Environment]::GetFolderPath("ProgramFilesX86")) "Git\usr\bin\openssl.exe")
  )

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) {
      return Get-Item -LiteralPath $candidate
    }
  }

  $fromPath = Get-Command openssl.exe -ErrorAction SilentlyContinue
  if ($fromPath) {
    return Get-Item -LiteralPath $fromPath.Source
  }

  return $null
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

function Add-CertificateToStore {
  param(
    [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
    [string]$StoreName
  )

  $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, "CurrentUser")
  $store.Open("ReadWrite")
  try {
    $store.Add($Certificate)
  } finally {
    $store.Close()
  }
}

function Remove-CertificateFromStore {
  param(
    [string]$Thumbprint,
    [string]$StoreName
  )

  $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, "CurrentUser")
  $store.Open("ReadWrite")
  try {
    foreach ($match in @($store.Certificates.Find("FindByThumbprint", $Thumbprint, $false))) {
      $store.Remove($match)
    }
  } finally {
    $store.Close()
  }
}

function New-TestSigningCertificateFiles {
  param(
    [string]$OpenSslPath,
    [string]$OutputDirectory,
    [string]$CertificateSubject,
    [int]$CertificateValidDays,
    [string]$PfxPassword,
    [string]$PublicCertPath
  )

  $commonName = $CertificateSubject
  if ($commonName.StartsWith("CN=")) {
    $commonName = $commonName.Substring(3)
  }

  $configPath = Join-Path $OutputDirectory "openssl-code-signing.cnf"
  $keyPath = Join-Path $OutputDirectory "melearner-test-signing.key"
  $pemPath = Join-Path $OutputDirectory "melearner-test-signing.pem"
  $pfxPath = Join-Path $OutputDirectory "melearner-test-signing.pfx"

  $config = @"
[ req ]
distinguished_name = req_distinguished_name
x509_extensions = v3_req
prompt = no

[ req_distinguished_name ]
CN = $commonName

[ v3_req ]
basicConstraints = critical,CA:true
keyUsage = critical,digitalSignature
extendedKeyUsage = codeSigning
"@

  Set-Content -LiteralPath $configPath -Value $config -Encoding ascii

  Invoke-Checked `
    -Description "creating per-run self-signed test code-signing certificate" `
    -Command $OpenSslPath `
    -Arguments @("req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", "$CertificateValidDays", "-keyout", $keyPath, "-out", $pemPath, "-config", $configPath)

  Invoke-Checked `
    -Description "exporting temporary test signing PFX" `
    -Command $OpenSslPath `
    -Arguments @("pkcs12", "-export", "-out", $pfxPath, "-inkey", $keyPath, "-in", $pemPath, "-passout", "pass:$PfxPassword")

  Invoke-Checked `
    -Description "exporting public test signing certificate" `
    -Command $OpenSslPath `
    -Arguments @("x509", "-in", $pemPath, "-outform", "DER", "-out", $PublicCertPath)

  return $pfxPath
}

if (!(Test-Path -LiteralPath $MsiDirectory)) {
  throw "MSI directory does not exist: $MsiDirectory"
}

Write-Output "finding Windows SDK signtool.exe"
$signTool = Find-SignTool
if (!$signTool) {
  throw "could not find Windows SDK signtool.exe; install the Windows SDK Signing Tools"
}
Write-Output "using signtool at $($signTool.FullName)"

Write-Output "finding Git OpenSSL"
$openSsl = Find-OpenSsl
if (!$openSsl) {
  throw "could not find openssl.exe"
}
Write-Output "using OpenSSL at $($openSsl.FullName)"

$msiFiles = @(Get-ChildItem -LiteralPath $MsiDirectory -Filter "*.msi" -File)
if ($msiFiles.Count -lt 1) {
  throw "no MSI files found in $MsiDirectory"
}
Write-Output "found $($msiFiles.Count) MSI file(s) in $MsiDirectory"

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) "melearner-test-signing-$([System.Guid]::NewGuid().ToString("N"))"
$pfxPassword = [System.Guid]::NewGuid().ToString("N")
$publicCertPath = Join-Path $MsiDirectory "melearner-windows-msi-test-signing.cer"

New-Item -ItemType Directory -Path $tempDir | Out-Null

$cert = $null
try {
  $pfxPath = New-TestSigningCertificateFiles `
    -OpenSslPath $openSsl.FullName `
    -OutputDirectory $tempDir `
    -CertificateSubject $Subject `
    -CertificateValidDays $ValidDays `
    -PfxPassword $pfxPassword `
    -PublicCertPath $publicCertPath

  $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($publicCertPath)
  Add-CertificateToStore -Certificate $cert -StoreName "Root"
  Add-CertificateToStore -Certificate $cert -StoreName "TrustedPublisher"
  Write-Output "exported public test certificate to $publicCertPath"

  foreach ($msi in $msiFiles) {
    Invoke-Checked `
      -Description "signing $($msi.FullName)" `
      -Command $signTool.FullName `
      -Arguments @("sign", "/f", $pfxPath, "/p", $pfxPassword, "/fd", "SHA256", "/v", $msi.FullName)

    Invoke-Checked `
      -Description "verifying $($msi.FullName)" `
      -Command $signTool.FullName `
      -Arguments @("verify", "/pa", "/v", $msi.FullName)

    Write-Output "signed $($msi.FullName) with $Subject ($($cert.Thumbprint))"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
  if ($cert) {
    foreach ($storeName in @("Root", "TrustedPublisher")) {
      Remove-CertificateFromStore -Thumbprint $cert.Thumbprint -StoreName $storeName
    }
  }
}
