# Test stand, as SYSTEM (StandStress task, see run-stress section in docs/test-stand.md):
# random play/pause/volume and occasional device/service restarts for -Minutes,
# with Driver Verifier on LdacA2dp.sys. Log: C:\stand\stress.log.
param([int]$Minutes = 40, [string]$Wav = 'C:\stand\beeps.wav')
$ErrorActionPreference = 'Continue'
$log = 'C:\stand\stress.log'
function Log($m) { "{0:HH:mm:ss} {1}" -f (Get-Date), $m | Out-File $log -Append -Encoding utf8 }

. C:\stand\set-volume.ps1 -Percent 60 | Out-Null   # loads [Vol]
Add-Type -AssemblyName PresentationCore
$player = $null
function Play {
    if ($script:player) { $script:player.Close() }
    $script:player = New-Object System.Windows.Media.MediaPlayer
    $script:player.Open([Uri]$Wav)
    Start-Sleep -Milliseconds 500
    $script:player.Play()
}
function DeviceId($uuid) { (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "BTHENUM\{$uuid*" }).InstanceId }

$end = (Get-Date).AddMinutes($Minutes)
$nextStatus = Get-Date
$rnd = New-Object Random
$counts = @{}
Log "stress start, $Minutes min"
while ((Get-Date) -lt $end) {
    $r = $rnd.Next(100)
    $action = if ($r -lt 30) { 'play' } elseif ($r -lt 45) { 'pause' } elseif ($r -lt 55) { 'resume' }
              elseif ($r -lt 80) { 'volume' } elseif ($r -lt 87) { 'stop' } elseif ($r -lt 92) { 'restart-a2dp' }
              elseif ($r -lt 96) { 'restart-avrcp' } else { 'restart-service' }
    try {
        switch ($action) {
            'play'   { Play }
            'pause'  { if ($player) { $player.Pause() } }
            'resume' { if ($player) { $player.Play() } }
            'stop'   { if ($player) { $player.Close(); $player = $null } }
            'volume' { $v = $rnd.Next(5, 101); [Vol]::Set($v / 100) | Out-Null; $action += " $v%" }
            'restart-a2dp'    { pnputil /restart-device (DeviceId '0000110B') | Out-Null }
            'restart-avrcp'   { pnputil /restart-device (DeviceId '0000110E') | Out-Null }
            'restart-service' { Restart-Service LdacSvc }
        }
        Log $action
    } catch { Log "$action failed: $($_.Exception.Message)" }
    $counts[$action.Split(' ')[0]] = 1 + [int]$counts[$action.Split(' ')[0]]
    if ((Get-Date) -ge $nextStatus) {
        $s = & C:\stand\ldacsvc.exe status 2>&1 | Out-String
        Log ("status`n" + $s.TrimEnd())
        $nextStatus = (Get-Date).AddMinutes(1)
    }
    Start-Sleep -Seconds $rnd.Next(10, 26)
}
if ($player) { $player.Close() }
Log ("stress done: " + (($counts.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' '))
