<#
.SYNOPSIS
  Removes LDAC A2DP: the LdacSvc service, the LdacA2dp driver packages, the
  files, the Start menu shortcut, the Installed apps entry and the package's
  test certificate. The headphones go back to the Windows drivers.

.PARAMETER RemoveSettings
  Also delete %ProgramData%\LdacA2dp (settings, status and the service log).
#>
param([switch] $RemoveSettings, [switch] $Quiet)
$ErrorActionPreference = 'Continue'   # remove as much as possible
$dst = Join-Path $env:ProgramFiles 'LdacA2dp'
$arpKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LdacA2dp'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run uninstall.cmd (it asks for administrator rights).' }

Step 'Removing the LdacSvc service'
$svc = Join-Path $dst 'ldacsvc.exe'
if (Get-Service LdacSvc -ErrorAction SilentlyContinue) {
    if (Test-Path $svc) { & $svc uninstall } else { Stop-Service LdacSvc -Force; sc.exe delete LdacSvc | Out-Null }
}
Get-Process LdacConfig -ErrorAction SilentlyContinue | Stop-Process -Force

Step 'Removing the driver packages (the headphones go back to the Windows drivers)'
$list = pnputil /enum-drivers | Out-String
foreach ($block in ($list -split "(\r?\n){2,}")) {
    if ($block -match '(?i)ldaca2dp\.inf' -and $block -match '(oem\d+\.inf)') {
        $inf = $Matches[1]
        Write-Host "  $inf"
        pnputil /delete-driver $inf /uninstall /force | Out-Null
    }
}
pnputil /scan-devices | Out-Null

Step 'Removing the test certificate'
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq 'CN=LdacA2dp Test' } | Remove-Item -ErrorAction SilentlyContinue
}

Step 'Removing files, shortcut and the Installed apps entry'
Remove-Item (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\LDAC A2DP.lnk') -ErrorAction SilentlyContinue
Remove-Item $arpKey -Recurse -ErrorAction SilentlyContinue
# this script may run from $dst: remove the rest once it has exited
if ($PSScriptRoot -eq $dst) {
    Start-Process cmd.exe -ArgumentList "/c timeout /t 2 >nul & rmdir /s /q `"$dst`"" -WindowStyle Hidden
} else {
    Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
}
if ($RemoveSettings) {
    Remove-Item (Join-Path $env:ProgramData 'LdacA2dp') -Recurse -Force -ErrorAction SilentlyContinue
} else {
    Write-Host "Settings kept in $env:ProgramData\LdacA2dp (uninstall.ps1 -RemoveSettings deletes them)."
}

Step 'Done'
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^BTHENUM\\\{0000110[BCE]' } | ForEach-Object {
    $s = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
    '  {0,-6} {1,-34} {2}' -f $_.Status, $s, $_.FriendlyName
}
