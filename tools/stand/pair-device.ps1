<#
.SYNOPSIS
  Finds a Bluetooth device by name (it must be in pairing mode) and pairs it.
  Runs inside the test VM, e.g. via VBoxManage guestcontrol.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File pair-device.ps1 -Name "JBL Live 780NC"
#>
param([Parameter(Mandatory)] [string] $Name, [int] $Seconds = 25)
$ErrorActionPreference = 'Stop'
Start-Transcript -Path (Join-Path $PSScriptRoot 'pair.log') -Force | Out-Null
[Console]::OutputEncoding = [Text.Encoding]::UTF8

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
    Select-Object -First 1
function Await($op, [Type] $type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null

$selector = [Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false)
$deadline = (Get-Date).AddSeconds($Seconds)
$target = $null
while (-not $target -and (Get-Date) -lt $deadline) {
    $found = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
    foreach ($d in $found) { Write-Host ("seen: {0}" -f $d.Name) }
    $target = $found | Where-Object { $_.Name -like "*$Name*" } | Select-Object -First 1
}
if (-not $target) { throw "'$Name' not found. Is it in pairing mode?" }

Write-Host "pairing $($target.Name) ($($target.Id))"
$result = Await ($target.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
Write-Host "pairing status: $($result.Status), protection: $($result.ProtectionLevelUsed)"
if ($result.Status -ne 'Paired' -and $result.Status -ne 'AlreadyPaired') { exit 1 }
