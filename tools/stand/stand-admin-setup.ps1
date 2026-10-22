# Test VM only. Run once elevated: registers the StandAdmin task, which runs
# C:\stand\admin.ps1 as SYSTEM (output: C:\stand\admin.log). The tester account
# may start it, so later admin steps go through guestcontrol without a UAC prompt:
#   schtasks /run /tn StandAdmin
$ts = New-Object -ComObject Schedule.Service
$ts.Connect()
$def = $ts.NewTask(0)
$def.Settings.ExecutionTimeLimit = 'PT10M'
$def.Settings.DisallowStartIfOnBatteries = $false
$a = $def.Actions.Create(0)
$a.Path = 'C:\Windows\System32\cmd.exe'
$a.Arguments = '/c powershell -NoProfile -ExecutionPolicy Bypass -File C:\stand\admin.ps1 > C:\stand\admin.log 2>&1'
# SYSTEM and administrators: full; authenticated users: read and run.
$sddl = 'D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;AU)'
$ts.GetFolder('\').RegisterTaskDefinition('StandAdmin', $def, 6, 'SYSTEM', $null, 5, $sddl) | Out-Null
'StandAdmin registered' | Out-File C:\stand\admin-setup.log
