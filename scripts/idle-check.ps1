Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class IdleProbe {
  [StructLayout(LayoutKind.Sequential)]
  public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
  [DllImport("kernel32.dll")] public static extern uint GetTickCount();
  public static uint IdleMs() {
    LASTINPUTINFO li = new LASTINPUTINFO();
    li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
    GetLastInputInfo(ref li);
    return GetTickCount() - li.dwTime;   // uint wrap handles rollover
  }
  public static uint LastTick() {
    LASTINPUTINFO li = new LASTINPUTINFO();
    li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
    GetLastInputInfo(ref li);
    return li.dwTime;
  }
}
"@
Write-Output ("idleMs=" + [IdleProbe]::IdleMs() + " (" + [Math]::Round([IdleProbe]::IdleMs()/60000.0,1) + " min) lastInputTick=" + [IdleProbe]::LastTick() + " nowTick=" + [IdleProbe]::GetTickCount())
