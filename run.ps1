# Plays from run\: starts build\msr-native.exe there with the options the emulator-based build's launcher passes, and
# starts it again when its menu's Restart asks (exit code 75). More msr-native options go after the named ones.
#   .\run.ps1 [-Resolution 1..10] [-Fps 30|60|90|120|144] [-Fullscreen] [-NoVsync] [msr-native options...]
param([int]$Resolution = 9, [int]$Fps = 0, [switch]$Fullscreen, [switch]$NoVsync,
      [Parameter(ValueFromRemainingArguments = $true)] [string[]]$Rest)
$root = $PSScriptRoot
$exe = Join-Path $root 'build\msr-native.exe'
$run = Join-Path $root 'run'
if (-not (Test-Path $exe)) { throw "No build\msr-native.exe: cmake -S native -B build -G Ninja; cmake --build build -j 8" }
if (-not (Test-Path (Join-Path $run 'user\msr\recomp\msr_aot.dll'))) { throw "No run\user\msr\recomp\msr_aot.dll: see README (The game's code)" }
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH   # SDL2, libpng, zlib, the C++ runtime
$opts = @('--layout=single', '--scale=3', "--resolution=$Resolution", '--filter=fsr')
if ($Fps) { $opts += "--fps=$Fps" }
if ($Fullscreen) { $opts += '--fullscreen=borderless' }
if ($NoVsync) { $opts += '--vsync=off' }
if ($Rest) { $opts += $Rest }
do {
    $p = Start-Process -FilePath $exe -WorkingDirectory $run -ArgumentList $opts -PassThru -NoNewWindow `
                       -RedirectStandardError (Join-Path $run 'msr-native.log')
    $null = $p.Handle   # kept, or PowerShell loses the exit code
    $p.WaitForExit()
} while ($p.ExitCode -eq 75)
exit $p.ExitCode
