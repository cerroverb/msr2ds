param([string]$Tag = 'uc', [string]$Steps = '', [int]$Fps = 60, [int]$Boot = 20, [double]$Scale = 0.5,
      [string]$Exe = '', [string]$TestDir = '')
# Boots the test copy (MSR_TEST=1: keyboard profile, settings kept for tests) with the freshly
# built exe and walks $Steps, comma-separated:
#   Key or Key:holdMs   a button from $K in drive.ps1 (focuses the window first)
#   down:Key / up:Key   hold and release
#   tap:fx:fy           a click at fractions of the client area
#   shot                capture to $SP\<Tag>_NN.png
#   wait<ms>            pause
#   trig                dump the next bottom-screen readback (needs $env:MSR_DEV = 1)
#   seq<N>              dump N consecutive readbacks with their emulated time (MSR_DEV = 1);
#                       tools/msr/dev/bottom_classify.py reads them
# Example: ui_check.ps1 -Tag hud -Steps "A,wait4500,A,wait3000,A,wait22000,Right:700,shot"
# (title, file select, load slot A, then play; emulated time runs behind real time early on).
. (Join-Path $PSScriptRoot 'drive.ps1')
if (-not $Exe) { $Exe = Join-Path $Root 'build-windows\bin\Release\SamusReturns.exe' }
if ($TestDir) { $global:TB = $TestDir }
Stop-Game; Start-Sleep -Seconds 1
Copy-Item $Exe (Join-Path $TB 'SamusReturns.exe') -Force
$ini = Join-Path $TB 'user\msr\settings.ini'
$kept = @([System.IO.File]::ReadAllLines($ini) | Where-Object { $_ -notmatch '^frame_rate=' })
$out = @(); foreach ($l in $kept) { $out += $l; if ($l -eq '[General]') { $out += "frame_rate=$Fps" } }
[System.IO.File]::WriteAllLines($ini, [string[]]$out)
$env:MSR_TEST = '1'
$p = Start-Game
Remove-Item Env:MSR_TEST
Start-Sleep -Seconds $Boot
Focus-Game
$n = 0
foreach ($step in $Steps.Split(',', [System.StringSplitOptions]::RemoveEmptyEntries)) {
  if ($step -eq 'shot') { Shot ('{0}_{1:D2}' -f $Tag, $n) $Scale | Out-Null; $n++ }
  elseif ($step -like 'wait*') { Start-Sleep -Milliseconds ([int]$step.Substring(4)) }
  elseif ($step -like 'seq*') { Set-Content -Path (Join-Path $TB 'user\msr\bottomseq.trigger') -Value $step.Substring(3) -Encoding ascii }
  elseif ($step -eq 'trig') { New-Item -ItemType File (Join-Path $TB 'user\msr\bottom.trigger') -Force | Out-Null; Start-Sleep -Milliseconds 800 }
  elseif ($step -like 'down:*') { Focus-Game; Key-Down $K[$step.Substring(5)] }
  elseif ($step -like 'up:*') { Key-Up $K[$step.Substring(3)] }
  elseif ($step -like 'tap*') { $t = $step.Split(':'); Tap ([double]$t[1]) ([double]$t[2]) 300 }
  else {
    $parts = $step.Split(':'); $hold = if ($parts.Count -gt 1) { [int]$parts[1] } else { 150 }
    Focus-Game; Press $parts[0] $hold
  }
}
if (Get-TestProc) { 'running' } else { 'EXITED' }
Stop-Game
"$n shots in $SP"
