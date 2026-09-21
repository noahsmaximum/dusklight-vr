# Dev harness: launch the portable test copy of Dusklight with the freshly built mod, wait, and
# capture the game window without touching input focus (PrintWindow).
param(
    [int]$Seconds = 60,
    [string]$Stage = "F_SP103",
    [string[]]$Cvars = @("mod.com_noahsmaximum_dusklight__vr.simulateHmd=true", "game.enableFrameInterpolation=2"),
    [string]$Shot = "$PSScriptRoot\..\build\shot.png",
    [switch]$KeepRunning
)
$root = Resolve-Path "$PSScriptRoot\.."
$game = Join-Path $root "testgame"
$iso = "C:\Users\Noah\ROMs\GameCube\Legend of Zelda, The - Twilight Princess (USA) w Linkle.iso"
Get-Process dusklight -ErrorAction SilentlyContinue | Stop-Process -Force
$argList = @('--dvd', "`"$iso`"", '--mods', "`"$(Join-Path $root 'build\mods')`"")
if ($Stage) { $argList += @('--stage', $Stage) }
foreach ($c in $Cvars) { $argList += @('--cvar', $c) }
$p = Start-Process -FilePath (Join-Path $game 'dusklight.exe') -WorkingDirectory $game -ArgumentList $argList `
    -RedirectStandardOutput (Join-Path $game 'out.log') -RedirectStandardError (Join-Path $game 'err.log') -PassThru
Start-Sleep $Seconds

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class PW {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct RECT { public int L, T, R, B; }
}
'@
$p.Refresh()
if ($p.HasExited) { Write-Output "GAME EXITED with code $($p.ExitCode)"; return }
$h = $p.MainWindowHandle
$r = New-Object PW+RECT
[PW]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc(); [PW]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
$bmp.Save($Shot)
if (-not $KeepRunning) { Stop-Process -Id $p.Id -Force }
Write-Output "captured $Shot"
