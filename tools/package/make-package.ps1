<#
.SYNOPSIS
  Builds the LDAC A2DP installation package: build\package\LdacA2dp-<version>\
  and the same as a .zip.

.DESCRIPTION
  1. service and tests (build.cmd), Release driver (build_driver.cmd Release);
  2. test-signs the driver (tools\stand\sign-driver.ps1) unless -NoSign;
  3. publishes the settings utility as one self-contained exe (the target
     machine may have no .NET 10);
  4. lays out installer\* next to them, writes version.txt (the driver's
     DriverVer version) and zips the folder.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package\make-package.ps1
#>
param([switch] $NoSign)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Run($exe, [string[]] $argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe $($argv -join ' ') failed ($LASTEXITCODE)" }
}

Step 'Service and tests'
Run cmd.exe @('/c', "`"$root\build.cmd`"")
Step 'Driver (Release)'
Run cmd.exe @('/c', "`"$root\build_driver.cmd`" Release")
$drv = Join-Path $root 'build\LdacA2dp\Release\LdacA2dp'
if (-not $NoSign) {
    Step 'Test signature'
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\stand\sign-driver.ps1" -Configuration Release
    if ($LASTEXITCODE -ne 0) { throw 'sign-driver.ps1 failed' }
}

Step 'Settings utility (self-contained single exe)'
$pub = Join-Path $root 'build\LdacConfig-publish'
Run dotnet @('publish', "$root\ui\LdacConfig", '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
             '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', '-p:EnableCompressionInSingleFile=true', '-o', $pub, '--nologo')

$version = ((Select-String -Path "$drv\LdacA2dp.inf" -Pattern '^DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value)
$name = "LdacA2dp-$version"
$out = Join-Path $root "build\package\$name"
Step "Layout $out"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$out\driver" | Out-Null
foreach ($f in 'LdacA2dp.inf', 'LdacA2dp.sys', 'ldaca2dp.cat', 'LdacA2dpTest.cer') {
    if (Test-Path "$drv\$f") { Copy-Item "$drv\$f" "$out\driver" }
}
Copy-Item "$root\build\ldacsvc.exe" $out
Copy-Item "$pub\LdacConfig.exe" $out
Copy-Item "$root\installer\*" $out
Set-Content -Path "$out\version.txt" -Value $version -Encoding ascii

$zip = "$out.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Step 'Done'
Get-ChildItem $out -Recurse -File | Select-Object @{ n = 'File'; e = { $_.FullName.Substring($out.Length + 1) } }, Length | Format-Table -AutoSize
Write-Host "Package: $zip ($([int]((Get-Item $zip).Length / 1MB)) MB)"
