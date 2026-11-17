Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Pw3 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowW(string cls, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$prog = [Pw3]::FindWindowW('Progman', $null)
$found = [IntPtr]::Zero
$cb = [Pw3+EnumProc]{
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [void][Pw3]::GetClassNameW($h, $sb, 256)
  if ($sb.ToString() -eq 'NativeWallpaperWnd') { $script:found = $h; return $false }
  return $true
}
[void][Pw3]::EnumChildWindows($prog, $cb, [IntPtr]::Zero)
[GC]::KeepAlive($cb)
$hwnd = $script:found
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'window-not-found'; exit }
$r = New-Object 'Pw3+RECT'
[void][Pw3]::GetWindowRect($hwnd, [ref]$r)
$bw = $r.R - $r.L; $bh = $r.B - $r.T
Write-Output ("hwnd=$hwnd rect=" + $r.L + "," + $r.T + " " + $bw + "x" + $bh)
$bmp = [System.Drawing.Bitmap]::new($bw, $bh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Pw3]::PrintWindow($hwnd, $hdc, 2)
$g.ReleaseHdc($hdc); $g.Dispose()
if ($ok) { $bmp.Save('D:\Projects\NativeWallpaper\out\wallpaper-window.png', [System.Drawing.Imaging.ImageFormat]::Png); Write-Output 'captured' }
else { Write-Output ("PrintWindow failed err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
$bmp.Dispose()
