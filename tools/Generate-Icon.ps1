[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $root "docs\app-icon-source.png"
$assetDir = Join-Path $root "assets"
$pngPath = Join-Path $assetDir "ActiveTAG-Configurator.png"
$icoPath = Join-Path $assetDir "ActiveTAG-Configurator.ico"

New-Item -ItemType Directory -Path $assetDir -Force | Out-Null

$source = [Drawing.Bitmap]::new($sourcePath)
try {
    $left = $source.Width
    $top = $source.Height
    $right = 0
    $bottom = 0

    for ($y = 0; $y -lt $source.Height; $y += 2) {
        for ($x = 0; $x -lt $source.Width; $x += 2) {
            $pixel = $source.GetPixel($x, $y)
            if ($pixel.R -lt 245 -or $pixel.G -lt 245 -or $pixel.B -lt 245) {
                $left = [Math]::Min($left, $x)
                $top = [Math]::Min($top, $y)
                $right = [Math]::Max($right, $x)
                $bottom = [Math]::Max($bottom, $y)
            }
        }
    }

    if ($right -le $left -or $bottom -le $top) {
        throw "Icon subject bounds could not be detected."
    }

    $padding = [Math]::Max(4, [int](($right - $left) * 0.02))
    $left = [Math]::Max(0, $left - $padding)
    $top = [Math]::Max(0, $top - $padding)
    $right = [Math]::Min($source.Width - 1, $right + $padding)
    $bottom = [Math]::Min($source.Height - 1, $bottom + $padding)

    $side = [Math]::Max($right - $left + 1, $bottom - $top + 1)
    $cropX = [Math]::Max(0, [int](($left + $right - $side) / 2))
    $cropY = [Math]::Max(0, [int](($top + $bottom - $side) / 2))
    $side = [Math]::Min($side, [Math]::Min($source.Width - $cropX, $source.Height - $cropY))

    $master = [Drawing.Bitmap]::new(512, 512, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($master)
    try {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage(
            $source,
            [Drawing.Rectangle]::new(0, 0, 512, 512),
            [Drawing.Rectangle]::new($cropX, $cropY, $side, $side),
            [Drawing.GraphicsUnit]::Pixel
        )
    } finally {
        $graphics.Dispose()
    }

    $visited = [Collections.Generic.HashSet[int]]::new()
    $queue = [Collections.Generic.Queue[int]]::new()

    function Add-BackgroundPixel {
        param([int]$X, [int]$Y)
        $index = ($Y * 512) + $X
        if ($visited.Contains($index)) {
            return
        }
        $pixel = $master.GetPixel($X, $Y)
        $brightness = [Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
        if ($brightness -ge 190) {
            [void]$visited.Add($index)
            $queue.Enqueue($index)
        }
    }

    for ($coordinate = 0; $coordinate -lt 512; $coordinate++) {
        Add-BackgroundPixel -X $coordinate -Y 0
        Add-BackgroundPixel -X $coordinate -Y 511
        Add-BackgroundPixel -X 0 -Y $coordinate
        Add-BackgroundPixel -X 511 -Y $coordinate
    }

    while ($queue.Count -gt 0) {
        $index = $queue.Dequeue()
        $x = $index % 512
        $y = [int][Math]::Floor($index / 512)
        $master.SetPixel($x, $y, [Drawing.Color]::Transparent)

        if ($x -gt 0) { Add-BackgroundPixel -X ($x - 1) -Y $y }
        if ($x -lt 511) { Add-BackgroundPixel -X ($x + 1) -Y $y }
        if ($y -gt 0) { Add-BackgroundPixel -X $x -Y ($y - 1) }
        if ($y -lt 511) { Add-BackgroundPixel -X $x -Y ($y + 1) }
    }

    $master.Save($pngPath, [Drawing.Imaging.ImageFormat]::Png)

    $sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    $images = @()
    foreach ($size in $sizes) {
        $resized = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $resizeGraphics = [Drawing.Graphics]::FromImage($resized)
        try {
            $resizeGraphics.Clear([Drawing.Color]::Transparent)
            $resizeGraphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $resizeGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $resizeGraphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $resizeGraphics.DrawImage($master, 0, 0, $size, $size)
        } finally {
            $resizeGraphics.Dispose()
        }

        $stream = [IO.MemoryStream]::new()
        $resized.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $images += ,@($size, $stream.ToArray())
        $stream.Dispose()
        $resized.Dispose()
    }

    $file = [IO.File]::Open($icoPath, [IO.FileMode]::Create, [IO.FileAccess]::Write)
    $writer = [IO.BinaryWriter]::new($file)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$images.Count)

        $offset = 6 + (16 * $images.Count)
        foreach ($image in $images) {
            $size = [int]$image[0]
            $bytes = [byte[]]$image[1]
            $writer.Write([byte]($(if ($size -eq 256) { 0 } else { $size })))
            $writer.Write([byte]($(if ($size -eq 256) { 0 } else { $size })))
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$bytes.Length)
            $writer.Write([uint32]$offset)
            $offset += $bytes.Length
        }

        foreach ($image in $images) {
            $writer.Write([byte[]]$image[1])
        }
    } finally {
        $writer.Dispose()
        $file.Dispose()
    }

    $master.Dispose()
} finally {
    $source.Dispose()
}

Write-Host "Generated icon assets:"
Write-Host "  $pngPath"
Write-Host "  $icoPath"
