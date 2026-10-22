# Test stand: dump the texts of the LDAC A2DP settings window (UI Automation) to C:\stand\uia.txt.
# Run in the interactive session (schtasks /it), the window must be open.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'LDAC A2DP Driver Configuration')
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $win) { 'window not found' | Out-File C:\stand\uia.txt -Encoding utf8; return }
$r = $win.Current.BoundingRectangle
$lines = @("window $([int]$r.Width)x$([int]$r.Height)")
$texts = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)))
foreach ($t in $texts) { if ($t.Current.Name) { $lines += $t.Current.Name } }
$lines | Out-File C:\stand\uia.txt -Encoding utf8
