<#
.SYNOPSIS
  Installs LDAC A2DP: the LdacA2dp driver, the LdacSvc encoder service and the
  LdacConfig settings utility.

.DESCRIPTION
  Run elevated from the unpacked package folder (install.cmd does that).
    1. checks Windows 11 22H2+ (ACX 1.1) and, for a test-signed driver, that
       test signing is on: it never changes boot options itself;
    2. trusts the package's test certificate if there is one;
    3. copies the service and the utility to %ProgramFiles%\LdacA2dp;
    4. installs the driver package (pnputil) on the supported headphones;
    5. registers and starts LdacSvc for the headphones the driver took;
    6. adds a Start menu shortcut and an "Installed apps" entry.
  Uninstall: uninstall.cmd (or Settings > Apps).
#>
param([switch] $Quiet)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$dst = Join-Path $env:ProgramFiles 'LdacA2dp'
$arpKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LdacA2dp'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run install.cmd (it asks for administrator rights).' }

foreach ($f in 'driver\LdacA2dp.inf', 'driver\LdacA2dp.sys', 'driver\ldaca2dp.cat', 'ldacsvc.exe', 'LdacConfig.exe', 'uninstall.ps1', 'version.txt') {
    if (-not (Test-Path (Join-Path $src $f))) { throw "The package is incomplete: $f is missing." }
}
$version = (Get-Content (Join-Path $src 'version.txt') -Raw).Trim()

# ---- 1. requirements -----------------------------------------------------------
Step "LDAC A2DP $version"
$build = [Environment]::OSVersion.Version.Build
if ($build -lt 22621) { throw "Windows 11 22H2 or later is required (ACX 1.1); this is build $build." }

$cert = Join-Path $src 'driver\LdacA2dpTest.cer'
$signer = (Get-AuthenticodeSignature (Join-Path $src 'driver\ldaca2dp.cat')).SignerCertificate
$testSigned = (Test-Path $cert) -or ($signer -and $signer.Subject -eq 'CN=LdacA2dp Test')
if ($testSigned) {
    $bcd = bcdedit /enum '{current}' | Out-String
    if ($bcd -notmatch 'testsigning\s+(Yes|Да)') {
        throw @"
This build of the driver is signed with a test certificate, and test signing is off.
Windows will only load it with test signing on. If you accept that on this machine:
    bcdedit /set testsigning on      (administrator command prompt; with Secure Boot on, turn it off in UEFI first)
then restart and run install.cmd again. The installer does not change boot options itself.
"@
    }
}

# ---- 2. certificate -----------------------------------------------------------------
if (Test-Path $cert) {
    Step 'Trusting the package test certificate (LocalMachine Root + TrustedPublisher)'
    Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

# ---- 3. files -------------------------------------------------------------------------
Step "Copying files to $dst"
if (Get-Service LdacSvc -ErrorAction SilentlyContinue) {
    Stop-Service LdacSvc -Force -ErrorAction SilentlyContinue   # an upgrade: free ldacsvc.exe
}
Get-Process LdacConfig -ErrorAction SilentlyContinue | Stop-Process -Force
New-Item -ItemType Directory -Force -Path $dst | Out-Null
foreach ($f in 'ldacsvc.exe', 'LdacConfig.exe', 'uninstall.ps1', 'uninstall.cmd', 'version.txt', 'README.txt') {
    if (Test-Path (Join-Path $src $f)) { Copy-Item (Join-Path $src $f) $dst -Force }
}
Copy-Item (Join-Path $src 'driver') $dst -Recurse -Force

# ---- 4. driver ------------------------------------------------------------------------------
Step 'Installing the driver package'
$out = pnputil /add-driver (Join-Path $src 'driver\LdacA2dp.inf') /install | Out-String
Write-Host $out.Trim()
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010 -and $LASTEXITCODE -ne 259) { throw "pnputil failed ($LASTEXITCODE)" }
# 259: added, but no device took it (headphones not paired yet, or already on this version)
Start-Sleep -Seconds 5

$a2dp = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match '^BTHENUM\\\{0000110B' -and
    (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data -eq 'LdacA2dp' })
$addresses = @($a2dp | ForEach-Object { if ($_.InstanceId -match '([0-9A-F]{12})_C0+$') { $Matches[1] } } | Select-Object -Unique)
if ($addresses.Count -eq 0) {
    Write-Warning 'No paired headphones use the driver yet. Pair them (Settings > Bluetooth), then run install.cmd again or "pnputil /scan-devices".'
}

# ---- 5. service ---------------------------------------------------------------------------------
Step 'Registering the LdacSvc service'
$svcArgs = @('install')
if ($addresses.Count -eq 1) {
    $mac = ($addresses[0] -split '(..)' | Where-Object { $_ }) -join ':'
    $svcArgs += @('--address', $mac)
    Write-Host "Headphones: $mac"
} elseif ($addresses.Count -gt 1) {
    Write-Warning "Several headphones use the driver ($($addresses -join ', ')); the service serves the first one it finds."
}
& (Join-Path $dst 'ldacsvc.exe') @svcArgs
if ($LASTEXITCODE -ne 0) { throw "ldacsvc install failed ($LASTEXITCODE)" }

# ---- 6. shortcut and Installed apps entry -------------------------------------------------------
Step 'Start menu shortcut and the Installed apps entry'
$programs = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $programs 'LDAC A2DP.lnk'))
$lnk.TargetPath = Join-Path $dst 'LdacConfig.exe'
$lnk.WorkingDirectory = $dst
$lnk.Description = 'LDAC A2DP settings'
$lnk.Save()

New-Item -Path $arpKey -Force | Out-Null
$size = [int]((Get-ChildItem $dst -Recurse | Measure-Object Length -Sum).Sum / 1KB)
$values = @{
    DisplayName = 'LDAC A2DP'; DisplayVersion = $version; Publisher = 'LDAC A2DP Project'
    InstallLocation = $dst; DisplayIcon = (Join-Path $dst 'LdacConfig.exe')
    UninstallString = "`"$dst\uninstall.cmd`""
    QuietUninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$dst\uninstall.ps1`" -Quiet"
}
foreach ($k in $values.Keys) { New-ItemProperty -Path $arpKey -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
New-ItemProperty -Path $arpKey -Name EstimatedSize -Value $size -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $arpKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $arpKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null

Step 'Done'
& (Join-Path $dst 'ldacsvc.exe') status 2>$null | Out-Host
if (-not $Quiet) {
    Write-Host ''
    Write-Host 'LDAC A2DP is installed. Settings: Start menu > LDAC A2DP. Service log: %ProgramData%\LdacA2dp\ldacsvc.log'
}
