# Draws the PSDBhai app icon (a generic picture glyph) at several sizes
# and packs them into res\app.ico as PNG-compressed entries.
param([string]$Out = (Join-Path $PSScriptRoot '..\res\app.ico'))

Add-Type -AssemblyName System.Drawing

function New-RoundedPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconPng([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Background tile
    $bg = New-RoundedPath 0 0 ($s - 1) ($s - 1) ($s * 0.22)
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 19, 32, 58))), $bg)

    # Picture card
    $m = $s * 0.17
    $cw = $s - 2 * $m
    $card = New-RoundedPath $m ($m + $s * 0.04) $cw ($cw * 0.82) ($s * 0.08)
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 236, 244, 255))), $card)
    $g.SetClip($card)

    $top = $m + $s * 0.04
    $bottom = $top + $cw * 0.82
    # Sun
    $sun = $s * 0.13
    $g.FillEllipse((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 251, 191, 36))),
        ($m + $cw * 0.62), ($top + $cw * 0.12), $sun, $sun)
    # Mountains
    $back = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(($m + $cw * 0.30), $bottom)),
        (New-Object System.Drawing.PointF(($m + $cw * 0.66), ($top + $cw * 0.36))),
        (New-Object System.Drawing.PointF(($m + $cw * 1.05), $bottom)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 14, 165, 233))), $back)
    $front = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(($m - $cw * 0.05), $bottom)),
        (New-Object System.Drawing.PointF(($m + $cw * 0.32), ($top + $cw * 0.28))),
        (New-Object System.Drawing.PointF(($m + $cw * 0.72), $bottom)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 37, 99, 235))), $front)
    $g.ResetClip()
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

# Installer header images (32-bit BMP with alpha) for 100%, 150% and 200% DPI.
foreach ($s in 55, 83, 110) {
    $ms = New-Object System.IO.MemoryStream(, (New-IconPng $s))
    $img = [System.Drawing.Bitmap]::FromStream($ms)
    $bmpPath = Join-Path (Split-Path $Out) "wizard-small-$s.bmp"
    $img.Save($bmpPath, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $img.Dispose()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) { , (New-IconPng $s) }

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$fs = [System.IO.File]::Create($Out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Close()
"Wrote $Out ($((Get-Item $Out).Length) bytes)"
