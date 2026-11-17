Add-Type -AssemblyName System.Drawing
$exe = 'D:\\Projects\\NativeWallpaper\\out\\Agent-Heart.exe'
$i = [System.Drawing.Icon]::ExtractAssociatedIcon($exe)
Write-Output ('iconExtracted=' + ($null -ne $i))
if ($i) { Write-Output ('iconSize=' + $i.Width + 'x' + $i.Height) }
$v = (Get-Item $exe).VersionInfo
Write-Output ('FileDescription=' + $v.FileDescription)
Write-Output ('ProductName=' + $v.ProductName)
Write-Output ('FileVersion=' + $v.FileVersion)
Write-Output ('OriginalFilename=' + $v.OriginalFilename)
Write-Output ('bytes=' + (Get-Item $exe).Length)
