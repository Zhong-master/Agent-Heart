param([string]$Path)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile($Path)
$pts = @(@(1560,960), @(1560,1040), @(1600,1280), @(1600,1600), @(400,1400), @(2800,1900), @(1560,180))
foreach ($pt in $pts) {
  $p = $src.GetPixel($pt[0], $pt[1])
  Write-Output ("(" + $pt[0] + "," + $pt[1] + ") R=" + $p.R + " G=" + $p.G + " B=" + $p.B + " A=" + $p.A)
}
$src.Dispose()
