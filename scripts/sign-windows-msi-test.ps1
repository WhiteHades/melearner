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
    $pattern = Join-Path $root "Windows Kits\10\bin\*\x64\signtool.exe"
    $matches += @(Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue)
  }

  return $matches | Sort-Object FullName -Descending | Select-Object -First 1
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

if (!(Test-Path -LiteralPath $MsiDirectory)) {
  throw "MSI directory does not exist: $MsiDirectory"
}

$signTool = Find-SignTool
if (!$signTool) {
  throw "could not find Windows SDK signtool.exe; install the Windows SDK Signing Tools"
}

$msiFiles = @(Get-ChildItem -LiteralPath $MsiDirectory -Filter "*.msi" -File)
if ($msiFiles.Count -lt 1) {
  throw "no MSI files found in $MsiDirectory"
}

$cert = New-SelfSignedCertificate `
  -Type CodeSigningCert `
  -Subject $Subject `
  -KeyUsage DigitalSignature `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -NotAfter (Get-Date).AddDays($ValidDays)

try {
  Add-CertificateToStore -Certificate $cert -StoreName "Root"
  Add-CertificateToStore -Certificate $cert -StoreName "TrustedPublisher"

  $publicCertPath = Join-Path $MsiDirectory "melearner-windows-msi-test-signing.cer"
  Export-Certificate -Cert $cert -FilePath $publicCertPath -Force | Out-Null

  foreach ($msi in $msiFiles) {
    & $signTool.FullName sign /s My /sha1 $cert.Thumbprint /fd SHA256 /v $msi.FullName
    if ($LASTEXITCODE -ne 0) {
      throw "signtool failed to sign $($msi.FullName)"
    }

    & $signTool.FullName verify /pa /v $msi.FullName
    if ($LASTEXITCODE -ne 0) {
      throw "signtool failed to verify $($msi.FullName)"
    }

    $signature = Get-AuthenticodeSignature -FilePath $msi.FullName
    if ($signature.Status -ne "Valid") {
      throw "Authenticode verification for $($msi.FullName) was $($signature.Status)"
    }
    if ($signature.SignerCertificate.Subject -ne $Subject) {
      throw "MSI signer was $($signature.SignerCertificate.Subject), expected $Subject"
    }

    Write-Output "signed $($msi.FullName) with $Subject ($($cert.Thumbprint))"
  }

  Write-Output "exported public test certificate to $publicCertPath"
} finally {
  foreach ($storeName in @("Root", "TrustedPublisher", "My")) {
    Remove-CertificateFromStore -Thumbprint $cert.Thumbprint -StoreName $storeName
  }
}
