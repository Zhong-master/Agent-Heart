Add-Type -AssemblyName System.Drawing
$root = 'D:\\Projects\\NativeWallpaper'
$srcPath = Join-Path $root 'out\shot-scene.png'
$icoPath = Join-Path $root 'assets\wallpaper.ico'
$prevPath = Join-Path $root 'assets\wallpaper-preview.png'
New-Item -ItemType Directory -Force -Path (Join-Path $root 'assets') | Out-Null

$src = [System.Drawing.Bitmap]::FromFile($srcPath)
$min = [Math]::Min($src.Width, $src.Height)
$side = [int]($min * 0.38)
$sx = [int](($src.Width - $side) / 2)
$sy = [int](($src.Height - $side) / 2)
$rect = New-Object System.Drawing.Rectangle($sx, $sy, $side, $side)
$crop = $src.Clone($rect, $src.PixelFormat)
$src.Dispose()
Write-Output ('crop ' + $side + 'x' + $side + ' at ' + $sx + ',' + $sy)

function New-IconBitmap([int]$size) {
  $b = [System.Drawing.Bitmap]::new($size, $size)
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $r = [int][Math]::Max(2, ($size * 0.16))
  $p = [System.Drawing.Drawing2D.GraphicsPath]::new()
  $p.AddArc(0, 0, 2*$r, 2*$r, 180, 90)
  $p.AddArc($size - 2*$r, 0, 2*$r, 2*$r, 270, 90)
  $p.AddArc($size - 2*$r, $size - 2*$r, 2*$r, 2*$r, 0, 90)
  $p.AddArc(0, $size - 2*$r, 2*$r, 2*$r, 90, 90)
  $p.CloseFigure()
  $g.SetClip($p)
  $g.DrawImage($crop, 0, 0, $size, $size)
  $g.Dispose()
  $p.Dispose()
  return $b
}

$entries = @()
foreach ($s in 16, 32, 48, 64, 128, 256) {
  $b = New-IconBitmap $s
  if ($s -le 48) {
    $r2 = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $bd = $b.LockBits($r2, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $bd.Stride
    $all = New-Object byte[] ($stride * $s)
    [Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $all, 0, $all.Length)
    $b.UnlockBits($bd)
    $rowB = $s * 4
    $xor = New-Object byte[] ($rowB * $s)
    for ($i = 0; $i -lt $s; $i++) {
      [Array]::Copy($all, ($s - 1 - $i) * $stride, $xor, $i * $rowB, $rowB)
    }
    $maskStride = ([int][Math]::Ceiling($s / 4.0)) * 4
    $mask = New-Object byte[] ($maskStride * $s)
    $hdr = New-Object byte[] 40
    [BitConverter]::GetBytes([int]40).CopyTo($hdr, 0)
    [BitConverter]::GetBytes([int]$s).CopyTo($hdr, 4)
    [BitConverter]::GetBytes([int]($s * 2)).CopyTo($hdr, 8)
    [BitConverter]::GetBytes([int16]1).CopyTo($hdr, 12)
    [BitConverter]::GetBytes([int16]32).CopyTo($hdr, 14)
    [BitConverter]::GetBytes([int]($rowB * $s)).CopyTo($hdr, 20)
    $ms2 = New-Object System.IO.MemoryStream
    $ms2.Write($hdr, 0, 40)
    $ms2.Write($xor, 0, $xor.Length)
    $ms2.Write($mask, 0, $mask.Length)
    $entries += @{ w = $s; data = $ms2.ToArray() }
    $ms2.Dispose()
  } else {
    $ms3 = New-Object System.IO.MemoryStream
    $b.Save($ms3, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += @{ w = $s; data = $ms3.ToArray() }
    $ms3.Dispose()
  }
  if ($s -eq 256) { $b.Save($prevPath, [System.Drawing.Imaging.ImageFormat]::Png) }
  $b.Dispose()
}

$msOut = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($msOut)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$entries.Count)
$off = 6 + 16 * $entries.Count
foreach ($e in $entries) {
  $eb = if ([int]$e.w -ge 256) { [byte]0 } else { [byte]$e.w }
  $bw.Write($eb)
  $bw.Write($eb)
  $bw.Write([byte]0)
  $bw.Write([byte]0)
  $bw.Write([uint16]1)
  $bw.Write([uint16]32)
  $bw.Write([uint32]$e.data.Length)
  $bw.Write([uint32]$off)
  $off += $e.data.Length
}
foreach ($e in $entries) { $bw.Write($e.data) }
$bw.Flush()
[IO.File]::WriteAllBytes($icoPath, $msOut.ToArray())
$bw.Dispose()
$msOut.Dispose()
$ico = Get-Item $icoPath
Write-Output ('ico saved: ' + $ico.FullName + ' ' + $ico.Length + ' bytes, ' + $entries.Count + ' sizes')
