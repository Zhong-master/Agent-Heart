param([string]$Action)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class NativeInp {
  [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, UIntPtr dwExtraInfo);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
}
"@
function TapWin {
  [NativeInp]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60
  [NativeInp]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60
}
function ComboWinD {
  [NativeInp]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
  [NativeInp]::keybd_event(0x44, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
  [NativeInp]::keybd_event(0x44, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
  [NativeInp]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
}
switch ($Action) {
  "wind"   { ComboWinD; Write-Output "wind-done" }
  "tapwin" { TapWin; Write-Output "tapwin-done" }
  "wheel"  {
    $ok = [NativeInp]::SetCursorPos(1560, 1040)
    Start-Sleep -Milliseconds 250
    [NativeInp]::mouse_event(0x0800, 0, 0, 120, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    Write-Output ("wheel-done cursor=" + $ok)
  }
}
