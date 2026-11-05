# Test driver: runs build\msr-native.exe from test\ in the background (never the foreground, no console), captures its
# window after -Seconds, then posts -Keys (comma-separated steps) and captures again at each "shot", then stops it.
# Only the process it started is stopped: a game running from run\ (the same exe) is safe.
#   .\tools\dev\shot.ps1 -Seconds 14 -Shot tag -GameArgs '--mute --layout=single --scale=3 --resolution=2 --fps=60' -Keys '...'
# Steps:
#   46:200            key (VK code in hex) held 200 ms   (defaults: F=46 is A, 20 Space = B, 09 Tab = Start,
#                     57/41/53/44 WASD, 51 Q = L, 45 E = R, 10 Shift = Morph Ball, 43 C = next beam, 0D Enter = A, 70 F1)
#   down:VK / up:VK   press or release a key (hold several)
#   waitN             sleep N ms
#   shot              screenshot test\shots\<tag>_<n>.png (client area: coordinates as the mouse steps take them)
#   click:x:y[:ms]    left click at client x,y
#   ldown|lup|rdown|rup:x:y[:wParam]   mouse buttons (wParam = the buttons still held: 1 left, 2 right)
#   move:x:y[:wParam] mouse motion
#   wheel:delta       mouse wheel (120 = a notch up, -120 down)
#   close             close the window (the game's: the launcher comes back)
# The test folder is set up like run\ (user\msr\game-path.txt, user\msr\recomp\msr_aot.dll, user\native-save, ...).
# Posted mouse input: the background window gets WM_MOUSELEAVE, so SDL may put the pointer at 0,0 between steps.
# -Folder: another test folder (e.g. test-build\, an empty user folder for the launcher's first build)
# -Exe: another msr-native.exe, e.g. an unpacked release; it runs as a player's would: its own DLLs only (no MSYS2 in
#   PATH) and its own portable user folder beside it (no MSR_NATIVE_HOME); -Folder defaults to its folder
# -Direct: msr-native-direct.exe beside the program instead, without arguments (the launcher writes it once the native
#   code is built: the game without the launcher). The game is its grandchild (it runs msr-native.exe --direct).
param([int]$Seconds = 15, [string]$Shot = 'n', [string]$GameArgs = '--mute', [string]$Keys = '', [int]$After = 0, [string]$Folder = '',
      [string]$Exe = '', [switch]$Direct)
$root = Split-Path (Split-Path $PSScriptRoot)
$program = if ($Exe) { $Exe } else { Join-Path $root 'build\msr-native.exe' }   # not $exe: PowerShell names ignore case, that is -Exe
if ($Direct) { $program = Join-Path (Split-Path $program) 'msr-native-direct.exe'; $GameArgs = '' }
$run = if ($Folder) { $Folder } elseif ($Exe) { Split-Path $Exe } else { Join-Path $root 'test' }
$out = Join-Path $run 'shots'
New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -AssemblyName System.Drawing
if (-not ('ShotWin' -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class ShotWin {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 public static IntPtr Largest(uint pid) {
   IntPtr best = IntPtr.Zero; long area = 0;
   EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
     if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r);
       long a = (long)(r.R - r.L) * (r.B - r.T); if (a > area) { area = a; best = h; } }
     return true; }, IntPtr.Zero);
   return best; }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
 public struct RECT { public int L, T, R, B; } }
"@
}
[ShotWin]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null   # window sizes in real pixels (display scaling)
if ($Exe) { $env:PATH = (($env:PATH -split ';') | Where-Object { $_ -notlike '*msys64*' }) -join ';' }
else { $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH }
$env:SDL_MOUSE_FOCUS_CLICKTHROUGH = '1'          # posted clicks reach the window without focus
$env:SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN = '1'   # never takes the foreground
# -GameArgs '' starts the launcher (its user folder: test\, by MSR_NATIVE_HOME); its Play starts the game as a child,
# whose window the steps and shots then use
if ($Exe) { Remove-Item Env:MSR_NATIVE_HOME -ErrorAction SilentlyContinue } else { $env:MSR_NATIVE_HOME = $run }
$start = @{ FilePath = $program; WorkingDirectory = $run; PassThru = $true; NoNewWindow = $true; RedirectStandardError = (Join-Path $run 'shot.log') }
if ($GameArgs -ne '') { $start.ArgumentList = $GameArgs }
$p = Start-Process @start
# its children, theirs and so on, parents first (-Direct: msr-native.exe --direct, then the game it started)
function Kids { $all = @(); $level = @($p.Id)
  for ($depth = 0; $depth -lt 4 -and $level.Count; $depth++) {
    $next = @(foreach ($id in $level) { Get-CimInstance Win32_Process -Filter "ParentProcessId=$id" -ErrorAction SilentlyContinue | ForEach-Object { $_.ProcessId } })
    $all += $next; $level = $next }
  $all }
# the window to capture and send input to: the program's, else its descendants' (the game the launcher started)
function Target { $w = [ShotWin]::Largest([uint32]$p.Id); if ($w -eq [IntPtr]::Zero) { foreach ($k in Kids) { $w = [ShotWin]::Largest([uint32]$k); if ($w -ne [IntPtr]::Zero) { [ShotWin]::SetWindowPos($w, [IntPtr]1, 0, 0, 0, 0, 0x13) | Out-Null; break } } }; $w }
# behind every other window as soon as it exists (HWND_BOTTOM, no move/size/activation); captures use PrintWindow
for ($i = 0; $i -lt 100; $i++) { $bh = [ShotWin]::Largest([uint32]$p.Id); if ($bh -ne [IntPtr]::Zero) { [ShotWin]::SetWindowPos($bh, [IntPtr]1, 0, 0, 0, 0, 0x13) | Out-Null; break }; Start-Sleep -Milliseconds 50 }
Start-Sleep -Milliseconds 500; $bh = [ShotWin]::Largest([uint32]$p.Id); if ($bh -ne [IntPtr]::Zero) { [ShotWin]::SetWindowPos($bh, [IntPtr]1, 0, 0, 0, 0, 0x13) | Out-Null }
function Grab([string]$name) {
  $h = Target
  $r = New-Object ShotWin+RECT; [ShotWin]::GetClientRect($h, [ref]$r) | Out-Null
  $wr = New-Object ShotWin+RECT; [ShotWin]::GetWindowRect($h, [ref]$wr) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  if ($w -le 0) { "no window"; return }
  # the whole window, then its client area (so shot coordinates are client coordinates, as the mouse steps take them)
  $ww = $wr.R - $wr.L; $wh = $wr.B - $wr.T; $bx = [int](($ww - $w) / 2); $ty = $wh - $hh - $bx
  $full = New-Object System.Drawing.Bitmap $ww, $wh
  $g = [System.Drawing.Graphics]::FromImage($full); $hdc = $g.GetHdc()
  [ShotWin]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp = $full.Clone((New-Object System.Drawing.Rectangle $bx, $ty, $w, $hh), $full.PixelFormat); $full.Dispose()
  $bmp.Save((Join-Path $out "$name.png")); $bmp.Dispose()
  "saved $name ($w x $hh)"
}
function Key([int]$vk, [bool]$down) {
  $scan = [ShotWin]::MapVirtualKey($vk, 0)
  $ext = if ($vk -ge 0x21 -and $vk -le 0x2E) { 1 -shl 24 } else { 0 }   # arrows etc.: the extended-key flag, or SDL reads the keypad
  if ($down) { [ShotWin]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr](1 -bor ($scan -shl 16) -bor $ext)) | Out-Null }
  else { [ShotWin]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]([Int64](1 -bor ($scan -shl 16) -bor $ext -bor 0xC0000000))) | Out-Null }
}
Start-Sleep -Seconds $Seconds
if ($p.HasExited) { "exited early: $($p.ExitCode)" } else { Grab "${Shot}_0" }
if ($Keys -ne '' -and -not $p.HasExited) {
  $i = 1
  foreach ($k in $Keys.Split(',')) {
    $h = Target
    if ($k -like 'wait*') { Start-Sleep -Milliseconds ([int]$k.Substring(4)); continue }
    if ($k -eq 'shot') { Grab "${Shot}_$i"; $i++; continue }
    if ($k -eq 'close') { [ShotWin]::PostMessage($h, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 100; continue }
    if ($k -like 'down:*' -or $k -like 'up:*') { $c = $k.Split(':'); Key ([Convert]::ToInt32($c[1], 16)) ($c[0] -eq 'down'); Start-Sleep -Milliseconds 100; continue }
    if ($k -like 'wheel:*') { $c = $k.Split(':'); [ShotWin]::PostMessage($h, 0x020A, [IntPtr]([int]$c[1] -shl 16), [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 100; continue }
    if ($k -like 'move:*') { $c = $k.Split(':'); $wp = if ($c.Count -gt 3) { [int]$c[3] } else { 0 }; $lp = [IntPtr](([int]$c[2] -shl 16) -bor ([int]$c[1] -band 0xFFFF)); [ShotWin]::PostMessage($h, 0x0200, [IntPtr]$wp, $lp) | Out-Null; Start-Sleep -Milliseconds 100; continue }
    if ($k -match '^(l|r)(down|up):') { $c = $k.Split(':'); $lp = [IntPtr](([int]$c[2] -shl 16) -bor ([int]$c[1] -band 0xFFFF))
      $msg = @{ 'ldown' = 0x0201; 'lup' = 0x0202; 'rdown' = 0x0204; 'rup' = 0x0205 }[$c[0]]; $wp = @{ 'ldown' = 1; 'lup' = 0; 'rdown' = 2; 'rup' = 0 }[$c[0]]; if ($c.Count -gt 3) { $wp = [int]$c[3] }
      [ShotWin]::PostMessage($h, $msg, [IntPtr]$wp, $lp) | Out-Null; Start-Sleep -Milliseconds 100; continue }
    if ($k -like 'click:*') { $c = $k.Split(':'); $x = [int]$c[1]; $y = [int]$c[2]; $hold = if ($c.Count -gt 3) { [int]$c[3] } else { 150 }
      $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)); [ShotWin]::PostMessage($h, 0x0200, [IntPtr]0, $lp) | Out-Null; Start-Sleep -Milliseconds 250
      [ShotWin]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds $hold; [ShotWin]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null; Start-Sleep -Milliseconds 300; continue }
    $parts = $k.Split(':'); $vk = [Convert]::ToInt32($parts[0], 16); $hold = if ($parts.Count -gt 1) { [int]$parts[1] } else { 150 }
    Key $vk $true; Start-Sleep -Milliseconds $hold; Key $vk $false; Start-Sleep -Milliseconds 300
  }
  Start-Sleep -Seconds $After
  Grab "${Shot}_end"
}
$kids = Kids   # the launcher first (it would bring its window back or restart the game when it ends), then the game, by its id
if (-not $p.HasExited) { $p | Stop-Process }
foreach ($k in $kids) { Stop-Process -Id $k -ErrorAction SilentlyContinue }
