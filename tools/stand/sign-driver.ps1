<#
.SYNOPSIS
  Test-signs the LdacA2dp driver package with a self-signed certificate.

.DESCRIPTION
  Run on the development machine after build_driver.cmd. Creates (once) a
  code-signing certificate "CN=LdacA2dp Test" in CurrentUser\My, exports its
  public part to LdacA2dpTest.cer next to the package, and signs LdacA2dp.sys
  and ldaca2dp.cat with SHA-256.

  The certificate is trusted only where install-on-vm.ps1 imports it, i.e. on
  the test VM. Nothing here changes boot options or installs drivers.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\stand\sign-driver.ps1 -Configuration Release
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path "$PSScriptRoot\..\.."
$package = Join-Path $root "build\LdacA2dp\$Configuration\LdacA2dp"
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe'
$subject = 'CN=LdacA2dp Test'

if (-not (Test-Path "$package\LdacA2dp.sys")) { throw "No package at ${package}: run build_driver.cmd $Configuration first." }
if (-not (Test-Path $signtool)) { throw "signtool not found: $signtool" }

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating test certificate '$subject' in CurrentUser\My"
    $cert = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert -KeyAlgorithm RSA -KeyLength 3072 `
        -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3)
}
Export-Certificate -Cert $cert -FilePath "$package\LdacA2dpTest.cer" | Out-Null

# The catalog covers the .sys and .inf, so sign the binary first, then the catalog.
foreach ($file in "$package\LdacA2dp.sys", "$package\ldaca2dp.cat") {
    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My /v $file
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $file" }
}
Write-Host "Signed package: $package (certificate thumbprint $($cert.Thumbprint))"
