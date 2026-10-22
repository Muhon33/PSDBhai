# Builds every app image from the PSDBhai logo (res\brand\logo-1024.png):
#   res\app.ico                 app/installer icon, PNG-compressed entries 16-256 px
#   res\wizard-small-*.bmp      installer header images (100/150/200% DPI)
#   res\readme\logo.png         256 px logo for the README
param([string]$Source = (Join-Path $PSScriptRoot '..\res\brand\logo-1024.png'))

Add-Type -AssemblyName System.Drawing
$res = Join-Path $PSScriptRoot '..\res'
$logo = [System.Drawing.Image]::FromFile((Resolve-Path $Source))

function New-Scaled([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.SmoothingMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($logo, 0, 0, $s, $s)
    $g.Dispose()
    return $bmp
}

function Get-PngBytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return , $ms.ToArray()
}

# Installer header images
foreach ($s in 55, 83, 110) {
    $b = New-Scaled $s
    $b.Save((Join-Path $res "wizard-small-$s.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    $b.Dispose()
}

# README logo
New-Item -ItemType Directory -Force (Join-Path $res 'readme') | Out-Null
$b = New-Scaled 256
$b.Save((Join-Path $res 'readme\logo.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$b.Dispose()

# Icon
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) { $b = New-Scaled $s; , (Get-PngBytes $b); $b.Dispose() }
$out = Join-Path $res 'app.ico'
$fs = [System.IO.File]::Create($out)
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
$logo.Dispose()
"Wrote $out ($((Get-Item $out).Length) bytes), wizard images and README logo"
