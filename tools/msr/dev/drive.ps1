# Test driver for the Samus Returns build: launch the test copy, post input, capture its window.
# Dot-source it: . tools/msr/dev/drive.ps1
# It only ever touches the test copy in build-windows/test (never the playable bundle, which may
# be running at the same time), and keeps that copy muted.
Add-Type -AssemblyName System.Drawing
if (-not ([System.Management.Automation.PSTypeName]'Drv').Type) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Drv {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
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
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
 public struct RECT { public int L, T, R, B; } }
"@
}
[Drv]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$global:Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
# The test copy: SamusReturns.exe (copied in by ui_check.ps1) with its own user/ folder.
$global:TB = Join-Path $Root 'build-windows\test'
# Screenshots: $env:MSR_DEV_OUT, else build-windows\dev-out (ignored by git).
$global:SP = if ($env:MSR_DEV_OUT) { $env:MSR_DEV_OUT } else { Join-Path $Root 'build-windows\dev-out' }
New-Item -ItemType Directory -Force $SP | Out-Null

function Get-TestProc { Get-Process SamusReturns -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $TB 'SamusReturns.exe') } }
function Mute-TestCopy {
  # Raw lines, so non-ASCII button names (●) in the config are not re-encoded.
  $cfg = Join-Path $TB 'user\config\qt-config.ini'
  if (Test-Path $cfg) {
    $lines = [System.IO.File]::ReadAllLines($cfg) | ForEach-Object {
      if ($_ -match '^volume\\default=') { 'volume\default=false' }
      elseif ($_ -match '^volume=') { 'volume=0' }
      else { $_ }
    }
    [System.IO.File]::WriteAllLines($cfg, [string[]]$lines)
  }
}
function Start-Game([string[]]$args2 = @()) {
  Get-TestProc | Stop-Process
  Start-Sleep -Milliseconds 500
  Mute-TestCopy
  $exe = Join-Path $TB 'SamusReturns.exe'
  if ($args2.Count) { Start-Process -FilePath $exe -WorkingDirectory $TB -ArgumentList $args2 -PassThru }
  else { Start-Process -FilePath $exe -WorkingDirectory $TB -PassThru }
}
function Stop-Game { Get-TestProc | Stop-Process }
function Get-GameWindow { $p = Get-TestProc; if (-not $p) { throw 'test copy is not running' }; [Drv]::Largest([uint32]@($p)[0].Id) }
function Focus-Game { $h = Get-GameWindow; if ([Drv]::IsIconic($h)) { [Drv]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 500 }
  [Drv]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 200 }

function Send-Key([int]$vk, [int]$holdMs = 120, [IntPtr]$hwnd = [IntPtr]::Zero) {
  if ($hwnd -eq [IntPtr]::Zero) { $hwnd = Get-GameWindow }
  $scan = [Drv]::MapVirtualKey($vk, 0)
  $down = [IntPtr](1 -bor ($scan -shl 16))
  $up = [IntPtr]([Int64](1 -bor ($scan -shl 16) -bor 0xC0000000))
  [Drv]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, $down) | Out-Null
  Start-Sleep -Milliseconds $holdMs
  [Drv]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, $up) | Out-Null
  Start-Sleep -Milliseconds 80
}
# 3DS buttons on the keyboard profile the test copy uses (MSR_TEST=1). The pause menu drops
# presses shorter than about 0.3 s: hold 350 ms there.
$global:K = @{ A=0x41; B=0x53; X=0x5A; Y=0x58; L=0x51; R=0x57; ZL=0x31; ZR=0x32; Start=0x4D; Select=0x4E;
  DUp=0x54; DDown=0x47; DLeft=0x46; DRight=0x48; Up=0x26; Down=0x28; Left=0x25; Right=0x27;
  F1=0x70; Esc=0x1B; Tab=0x09; Enter=0x0D }
function Press([string]$name, [int]$holdMs = 120) { Send-Key $K[$name] $holdMs }

# Tap at a position given as fractions of the client area.
function Tap([double]$fx, [double]$fy, [int]$holdMs = 120) {
  $h = Get-GameWindow
  $c = New-Object Drv+RECT; [Drv]::GetClientRect($h, [ref]$c) | Out-Null
  $x = [int]($c.R * $fx); $y = [int]($c.B * $fy)
  $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
  [Drv]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null
  Start-Sleep -Milliseconds $holdMs
  [Drv]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null
}

# Captures the window with PrintWindow; scale < 1 shrinks the saved PNG ($SP\<name>.png).
function Shot([string]$name, [double]$scale = 0.34) {
  $h = Get-GameWindow
  $r = New-Object Drv+RECT; [Drv]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [Drv]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $sw = [int]($w * $scale); $sh = [int]($hh * $scale)
  $small = New-Object System.Drawing.Bitmap $sw, $sh
  $g2 = [System.Drawing.Graphics]::FromImage($small); $g2.InterpolationMode = 'HighQualityBicubic'
  $g2.DrawImage($bmp, 0, 0, $sw, $sh)
  $path = Join-Path $SP "$name.png"; $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose(); $small.Dispose()
  "$name ($w x $hh)"
}

function Key-Down([int]$vk) {
  $h = Get-GameWindow; $scan = [Drv]::MapVirtualKey($vk, 0)
  [Drv]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr](1 -bor ($scan -shl 16))) | Out-Null
}
function Key-Up([int]$vk) {
  $h = Get-GameWindow; $scan = [Drv]::MapVirtualKey($vk, 0)
  [Drv]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]([Int64](1 -bor ($scan -shl 16) -bor 0xC0000000))) | Out-Null
}
