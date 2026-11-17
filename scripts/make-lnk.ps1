$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut('D:\Projects\NativeWallpaper\out\launch.lnk')
$lnk.TargetPath = 'D:\Projects\NativeWallpaper\out\Agent-Heart.exe'
$lnk.Arguments = '--probe'
$lnk.WorkingDirectory = 'D:\Projects\NativeWallpaper\out'
$lnk.Save()
Write-Output 'lnk-ok'
