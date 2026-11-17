Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile('D:\Projects\NativeWallpaper\out\shot-tabtest.png')
foreach ($pt in @(@(99,50), @(245,50))) {
  $p = $src.GetPixel($pt[0], $pt[1])
  Write-Output ("(" + $pt[0] + "," + $pt[1] + ") R=" + $p.R + " G=" + $p.G + " B=" + $p.B)
}
$src.Dispose()
