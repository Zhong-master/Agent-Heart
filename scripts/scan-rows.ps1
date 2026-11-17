param([string]$Path)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile($Path)
Write-Output ("file=" + $Path + " dims=" + $src.Width + "x" + $src.Height)
$xs = 100, 400, 700, 1000, 1300, 1600, 1900, 2200, 2500, 2800, 3050
$bands = @()
for ($y = 0; $y -lt $src.Height; $y += 40) {
  $sum = 0; $n = 0
  foreach ($x in $xs) {
    $p = $src.GetPixel($x, [Math]::Min($y, $src.Height - 1))
    $lum = 0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B
    $sum += $lum; $n++
  }
  $avg = [Math]::Round($sum / $n, 1)
  if ($avg -gt 3) { $bands += ("y=" + $y + " avg=" + $avg) }
}
Write-Output ("nonblack rows: " + $bands.Count)
$bands | ForEach-Object { Write-Output $_ }
$src.Dispose()
