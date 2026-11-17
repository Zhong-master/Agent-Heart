param([string]$Path)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile($Path)
Write-Output ("file=" + $Path + " dims=" + $src.Width + "x" + $src.Height)
$ys = 960, 1040, 1280, 1500
foreach ($y in $ys) {
  if ($y -ge $src.Height) { continue }
  $line = "y=" + $y + ":"
  foreach ($x in 100, 400, 800, 1200, 1560, 1950, 2300, 2700, 3050) {
    if ($x -ge $src.Width) { continue }
    $p = $src.GetPixel($x, $y)
    $lum = [Math]::Round(0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B, 1)
    $line += " " + $x + "=" + $lum
  }
  Write-Output $line
}
$src.Dispose()
