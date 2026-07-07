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

function New-TestCodeSigningCertificate {
  param(
    [string]$CertificateSubject,
    [int]$CertificateValidDays
  )

  $rsa = [System.Security.Cryptography.RSA]::Create(2048)
  $distinguishedName = [System.Security.Cryptography.X509Certificates.X500DistinguishedName]::new($CertificateSubject)
  $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
    $distinguishedName,
    $rsa,
    [System.Security.Cryptography.HashAlgorithmName]::SHA256,
    [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
  )

  $request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true)
  )
  $request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
      [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,
      $true
    )
  )

  $codeSigningOids = [System.Security.Cryptography.OidCollection]::new()
  [void]$codeSigningOids.Add([System.Security.Cryptography.Oid]::new("1.3.6.1.5.5.7.3.3"))
  $request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($codeSigningOids, $false)
  )

  return $request.CreateSelfSigned(
    [System.DateTimeOffset]::UtcNow.AddMinutes(-5),
    [System.DateTimeOffset]::UtcNow.AddDays($CertificateValidDays)
  )
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

$pfxPath = Join-Path ([System.IO.Path]::GetTempPath()) "melearner-test-signing-$([System.Guid]::NewGuid().ToString("N")).pfx"
$pfxPassword = [System.Guid]::NewGuid().ToString("N")

Write-Output "creating per-run self-signed test code-signing certificate"
$cert = New-TestCodeSigningCertificate -CertificateSubject $Subject -CertificateValidDays $ValidDays

try {
  [System.IO.File]::WriteAllBytes($pfxPath, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $pfxPassword))

  $publicCertPath = Join-Path $MsiDirectory "melearner-windows-msi-test-signing.cer"
  $publicCertBytes = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
  [System.IO.File]::WriteAllBytes($publicCertPath, $publicCertBytes)
  $publicCert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($publicCertBytes)
  Add-CertificateToStore -Certificate $publicCert -StoreName "Root"
  Add-CertificateToStore -Certificate $publicCert -StoreName "TrustedPublisher"
  Write-Output "exported public test certificate to $publicCertPath"

  foreach ($msi in $msiFiles) {
    Write-Output "signing $($msi.FullName)"
    & $signTool.FullName sign /f $pfxPath /p $pfxPassword /fd SHA256 /v $msi.FullName
    if ($LASTEXITCODE -ne 0) {
      throw "signtool failed to sign $($msi.FullName)"
    }

    Write-Output "verifying $($msi.FullName)"
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
} finally {
  Remove-Item -LiteralPath $pfxPath -Force -ErrorAction SilentlyContinue
  foreach ($storeName in @("Root", "TrustedPublisher")) {
    Remove-CertificateFromStore -Thumbprint $cert.Thumbprint -StoreName $storeName
  }
}
