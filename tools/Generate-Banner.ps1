[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$productPath = Join-Path $root "docs\active-tag-hero.png"
$outputPath = Join-Path $root "docs\banner.png"

$width = 1400
$height = 420
$bitmap = [Drawing.Bitmap]::new($width, $height)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::ClearTypeGridFit

try {
    $background = [Drawing.Drawing2D.LinearGradientBrush]::new(
        [Drawing.Rectangle]::new(0, 0, $width, $height),
        [Drawing.Color]::FromArgb(7, 16, 21),
        [Drawing.Color]::FromArgb(18, 61, 54),
        22
    )
    $graphics.FillRectangle($background, 0, 0, $width, $height)
    $background.Dispose()

    $glow = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(34, 85, 230, 189))
    $graphics.FillEllipse($glow, 1050, -120, 480, 480)
    $graphics.FillEllipse($glow, 1110, 250, 330, 330)
    $glow.Dispose()

    $accent = [Drawing.Color]::FromArgb(85, 230, 189)
    $primary = [Drawing.Color]::FromArgb(243, 251, 248)
    $secondary = [Drawing.Color]::FromArgb(185, 203, 198)
    $muted = [Drawing.Color]::FromArgb(159, 179, 174)

    $eyebrowFont = [Drawing.Font]::new("Segoe UI", 15, [Drawing.FontStyle]::Bold)
    $titleFont = [Drawing.Font]::new("Segoe UI", 50, [Drawing.FontStyle]::Bold)
    $subtitleFont = [Drawing.Font]::new("Segoe UI", 28, [Drawing.FontStyle]::Regular)
    $bodyFont = [Drawing.Font]::new("Segoe UI", 15, [Drawing.FontStyle]::Regular)
    $badgeFont = [Drawing.Font]::new("Segoe UI", 11, [Drawing.FontStyle]::Bold)

    $accentBrush = [Drawing.SolidBrush]::new($accent)
    $primaryBrush = [Drawing.SolidBrush]::new($primary)
    $secondaryBrush = [Drawing.SolidBrush]::new($secondary)
    $mutedBrush = [Drawing.SolidBrush]::new($muted)

    $graphics.DrawString("NATIVE WINDOWS TOOL", $eyebrowFont, $accentBrush, 76, 64)
    $graphics.DrawString("ActiveTAG", $titleFont, $primaryBrush, 70, 115)
    $graphics.DrawString("Configurator", $subtitleFont, $secondaryBrush, 74, 206)
    $graphics.DrawString(
        "Read. Export. Override. Verify. Save.",
        $bodyFont,
        $mutedBrush,
        77,
        274
    )

    function Draw-Badge {
        param([int]$X, [int]$Width, [string]$Text)
        $badgeBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(21, 55, 47))
        $badgePen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(59, 141, 120), 1)
        $rectangle = [Drawing.Rectangle]::new($X, 334, $Width, 38)
        $graphics.FillRectangle($badgeBrush, $rectangle)
        $graphics.DrawRectangle($badgePen, $rectangle)
        $size = $graphics.MeasureString($Text, $badgeFont)
        $graphics.DrawString(
            $Text,
            $badgeFont,
            $accentBrush,
            $X + (($Width - $size.Width) / 2),
            343
        )
        $badgeBrush.Dispose()
        $badgePen.Dispose()
    }

    Draw-Badge -X 76 -Width 128 -Text "C++ / Win32"
    Draw-Badge -X 218 -Width 148 -Text "SINGLE EXE"
    Draw-Badge -X 380 -Width 180 -Text "NO WEB SERVER"

    $cardShadow = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(45, 85, 230, 189))
    $graphics.FillRectangle($cardShadow, 997, 38, 330, 330)
    $cardShadow.Dispose()

    $cardBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(3, 15, 18))
    $cardPen = [Drawing.Pen]::new($accent, 3)
    $graphics.FillRectangle($cardBrush, 1005, 46, 314, 314)
    $graphics.DrawRectangle($cardPen, 1005, 46, 314, 314)

    $product = [Drawing.Image]::FromFile($productPath)
    try {
        $graphics.DrawImage($product, [Drawing.Rectangle]::new(1012, 53, 300, 300))
    } finally {
        $product.Dispose()
    }

    $labelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(13, 37, 35))
    $graphics.FillRectangle($labelBrush, 1075, 330, 174, 38)
    $labelSize = $graphics.MeasureString("ACTIVE TAG", $badgeFont)
    $graphics.DrawString(
        "ACTIVE TAG",
        $badgeFont,
        $accentBrush,
        1075 + ((174 - $labelSize.Width) / 2),
        339
    )

    $bitmap.Save($outputPath, [Drawing.Imaging.ImageFormat]::Png)
} finally {
    foreach ($resource in @(
        $eyebrowFont, $titleFont, $subtitleFont, $bodyFont, $badgeFont,
        $accentBrush, $primaryBrush, $secondaryBrush, $mutedBrush,
        $cardBrush, $cardPen, $labelBrush
    )) {
        if ($null -ne $resource) {
            $resource.Dispose()
        }
    }
    $graphics.Dispose()
    $bitmap.Dispose()
}

Write-Host "Banner generated: $outputPath"
