Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class IlProbe {
  [DllImport("advapi32.dll", SetLastError=true)]
  static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr tok);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("advapi32.dll", SetLastError=true)]
  static extern bool GetTokenInformation(IntPtr tok, int cls, IntPtr info, int len, out int retLen);
  [DllImport("advapi32.dll")]
  static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
  [DllImport("advapi32.dll")]
  static extern IntPtr GetSidSubAuthority(IntPtr sid, int index);
  public static string Of(int pid) {
    IntPtr h = OpenProcess(0x1000, false, pid);   // QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) return "open-fail err=" + Marshal.GetLastWin32Error();
    IntPtr tok;
    if (!OpenProcessToken(h, 0x0008, out tok)) { CloseHandle(h); return "token-fail err=" + Marshal.GetLastWin32Error(); }
    int need;
    GetTokenInformation(tok, 25, IntPtr.Zero, 0, out need);   // TokenIntegrityLevel = 25
    IntPtr buf = Marshal.AllocHGlobal(need);
    string r;
    if (GetTokenInformation(tok, 25, buf, need, out need)) {
      // TOKEN_MANDATORY_LABEL { IntPtr Label; } -> SID at offset 0
      IntPtr sid = Marshal.ReadIntPtr(buf);
      byte count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
      int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, count - 1));
      string name = rid == 0x2000 ? "LOW" : rid == 0x4000 ? "MEDIUM" : rid == 0x5000 ? "HIGH" : rid >= 0x7000 ? "SYSTEM" : "0x" + rid.ToString("X");
      r = name + " (rid=0x" + rid.ToString("X") + ")";
    } else r = "info-fail err=" + Marshal.GetLastWin32Error();
    Marshal.FreeHGlobal(buf);
    CloseHandle(tok); CloseHandle(h);
    return r;
  }
}
"@
$me = (Get-Process -Id $PID).Id
$wp = (Get-Process -Name native-wallpaper -EA SilentlyContinue | Select-Object -First 1)
$ex = (Get-Process -Name explorer -EA SilentlyContinue | Select-Object -First 1)
Write-Output ("powershell(self) pid=$me IL=" + [IlProbe]::Of($me))
if ($wp) { Write-Output ("wallpaper pid=" + $wp.Id + " IL=" + [IlProbe]::Of($wp.Id)) } else { Write-Output "wallpaper: not running" }
if ($ex) { Write-Output ("explorer pid=" + $ex.Id + " IL=" + [IlProbe]::Of($ex.Id)) }
