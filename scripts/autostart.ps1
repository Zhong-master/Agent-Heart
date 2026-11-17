$ws = New-Object -ComObject WScript.Shell
$target = [Environment]::GetFolderPath('Startup') + '\Agent-Heart.lnk'
$lnk = $ws.CreateShortcut($target)
$lnk.TargetPath = 'D:\Projects\NativeWallpaper\out\Agent-Heart.exe'
$lnk.Arguments = ''
$lnk.WorkingDirectory = 'D:\Projects\NativeWallpaper\out'
$lnk.Save()
Write-Output ('created: ' + $target)
