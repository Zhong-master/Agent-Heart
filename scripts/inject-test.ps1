$log = 'D:\Projects\NativeWallpaper\out\inject-test.log'
if (Test-Path $log) { Remove-Item $log -Force }
function W($m) { Add-Content -Path $log -Value ((Get-Date -Format 'HH:mm:ss') + ' ' + $m) }
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Inp2 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
}
"@
W "injector started (interactive user token)"
$orig = [System.Windows.Forms.Cursor]::Position
$ok = [Inp2]::SetCursorPos(1560, 1040)
W ("SetCursorPos=" + $ok)
Start-Sleep -Milliseconds 300
[Inp2]::mouse_event(0x0800, 0, 0, 120, [UIntPtr]::Zero)
W "wheel delta=+120 sent (DOM: scroll up)"
Start-Sleep -Milliseconds 800
[Inp2]::SetCursorPos($orig.X, $orig.Y)
W "cursor restored; done"
