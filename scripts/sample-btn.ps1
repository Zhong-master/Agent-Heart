Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile('D:\Projects\NativeWallpaper\out\shot-editmode.png')
$p = $src.GetPixel(3056, 50)
Write-Output ("btn(3056,50) R=" + $p.R + " G=" + $p.G + " B=" + $p.B)
$src.Dispose()
