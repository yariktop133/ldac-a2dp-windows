<#
.SYNOPSIS
  Installs (or removes) the test-signed LdacA2dp driver on the TEST VM.

.DESCRIPTION
  Refuses to run unless test signing is already enabled, so it cannot be used
  by accident on a normal machine. Steps:
    1. trust LdacA2dpTest.cer (LocalMachine Root + TrustedPublisher),
    2. pnputil /add-driver LdacA2dp.inf /install,
    3. show which driver the headphones' A2DP device now uses.
  Run from an elevated PowerShell inside the VM, in the folder produced by
  tools\stand\make-bundle.cmd.

.EXAMPLE
  .\install-on-vm.ps1
  .\install-on-vm.ps1 -Uninstall
#>
param([switch] $Uninstall)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated.' }

$bcd = bcdedit /enum '{current}' | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') {
    throw "Test signing is off. On the TEST VM only: 'bcdedit /set testsigning on', reboot, run again."
}

function Show-Binding {
    Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^BTHENUM\\\{0000110B' } | ForEach-Object {
        $svc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service).Data
        $inf = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
        [pscustomobject]@{ Device = $_.FriendlyName; Status = $_.Status; Service = $svc; Inf = $inf }
    } | Format-Table -AutoSize
}

if ($Uninstall) {
    $published = pnputil /enum-drivers | Out-String
    $blocks = $published -split "(\r?\n){2,}" | Where-Object { $_ -match 'ldaca2dp\.inf' }
    foreach ($b in $blocks) {
        if ($b -match '(oem\d+\.inf)') {
            Write-Host "Removing $($Matches[1])"
            pnputil /delete-driver $Matches[1] /uninstall /force
        }
    }
    Show-Binding
    return
}

foreach ($f in 'LdacA2dp.inf', 'LdacA2dp.sys', 'ldaca2dp.cat', 'LdacA2dpTest.cer') {
    if (-not (Test-Path "$here\$f")) { throw "Missing $f next to this script (run sign-driver.ps1 and make-bundle.cmd)." }
}

Import-Certificate -FilePath "$here\LdacA2dpTest.cer" -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath "$here\LdacA2dpTest.cer" -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

pnputil /add-driver "$here\LdacA2dp.inf" /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }
Show-Binding
Write-Host 'If the headphones still use BthA2dp: pair them, then "pnputil /scan-devices" or reconnect them.'
