Add-Type -AssemblyName System.Windows.Forms
$i = 0
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
  $b = $s.Bounds
  Write-Output ("screen" + $i + ": " + $s.DeviceName + " loc=" + $b.X + "," + $b.Y + " size=" + $b.Width + "x" + $b.Height + " primary=" + $s.Primary)
  $i++
}
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Sm {
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
}
"@
Write-Output ("primary CX,CY=" + [Sm]::GetSystemMetrics(0) + "x" + [Sm]::GetSystemMetrics(1))
Write-Output ("virtual X,Y,CX,CY=" + [Sm]::GetSystemMetrics(76) + "," + [Sm]::GetSystemMetrics(77) + " " + [Sm]::GetSystemMetrics(78) + "x" + [Sm]::GetSystemMetrics(79))
