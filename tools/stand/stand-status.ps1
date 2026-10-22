# Test-stand snapshot: LdacSvc state, LdacA2dp device, tail of the service log.
$ErrorActionPreference = 'Continue'
Get-ChildItem C:\stand | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String -Width 200
Get-Service LdacSvc -ErrorAction SilentlyContinue | Format-List Name, Status, StartType | Out-String
Get-PnpDevice -PresentOnly | Where-Object { $_.Service -eq 'LdacA2dp' } |
    Format-Table Status, FriendlyName, InstanceId -AutoSize | Out-String -Width 300
$log = Join-Path $env:ProgramData 'LdacA2dp\ldacsvc.log'
if (Test-Path $log) { "---- $log"; $n = if ($args.Count) { [int]$args[0] } else { 40 }; Get-Content $log -Tail $n }
