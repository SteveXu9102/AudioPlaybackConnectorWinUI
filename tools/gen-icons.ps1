<#
.SYNOPSIS
    Builds the application icon (grey tile, white glyph) from the rounded SVG.

.DESCRIPTION
    Writes two assets:

      AudioPlaybackConnector.Rounded.ico   the application icon: a rounded grey
                                           tile carrying the glyph in white, at
                                           the five sizes the shipped icon uses
                                           (256, 48, 32, 24, 16).
      AudioPlaybackConnector.Tile.svg      the same artwork as SVG, for the panel
                                           badge, which WinUI draws itself.

    The glyph geometry comes from AudioPlaybackConnector.Rounded.svg, so the two
    icons cannot drift apart. That file only uses M, L and Z - absolute polygon
    subpaths - so it is parsed directly and filled with GDI+ using the winding
    rule, which is what SVG's default fill rule means. No SVG renderer is
    involved: nothing beyond .NET's System.Drawing is needed.

.EXAMPLE
    pwsh tools/gen-icons.ps1
#>
[CmdletBinding()]
param(
    [string]$Source = "$PSScriptRoot/../AudioPlaybackConnector.Rounded.svg",
    [string]$IcoPath = "$PSScriptRoot/../AudioPlaybackConnector.Rounded.ico",
    [string]$TilePath = "$PSScriptRoot/../AudioPlaybackConnector.Tile.svg",
    # The five sizes Windows' own application icons carry.
    [int[]]$Sizes = @(256, 48, 32, 24, 16),
    # Neutral grey tile, white glyph: legible on a light or a dark shell.
    [string]$TileColor = '#5C5C5C',
    [string]$GlyphColor = '#FFFFFF',
    # Fraction of the tile left empty around the glyph.
    [double]$Padding = 0.19,
    # Corner radius as a fraction of the tile size: Windows 11 application icons
    # are rounded squares.
    [double]$Radius = 0.22
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# The source SVG's own coordinate space.
$Canvas = 1536.0

function Get-SvgPathData {
    param([string]$Svg)
    $match = [regex]::Match($Svg, '<path[^>]*\sd="([^"]+)"')
    if (-not $match.Success) { throw 'The source SVG has no <path d="...">.' }
    $path = $match.Groups[1].Value
    if ($path -match '[AaCcQqSsHhVvTt]') {
        throw 'Only absolute M/L/Z polygon subpaths are supported.'
    }
    return $path
}

function Get-SvgPolygons {
    param([string]$PathData, [double]$ViewBox)

    $polygons = [System.Collections.Generic.List[object]]::new()
    foreach ($subpath in ($PathData -split '(?=M)')) {
        if ([string]::IsNullOrWhiteSpace($subpath)) { continue }

        $numbers = [System.Collections.Generic.List[double]]::new()
        foreach ($match in [regex]::Matches($subpath, '-?\d+(?:\.\d+)?')) {
            $numbers.Add([double]$match.Value)
        }
        if ($numbers.Count -lt 6 -or ($numbers.Count % 2) -ne 0) {
            throw "Unsupported subpath: $subpath"
        }

        $points = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
        for ($index = 0; $index -lt $numbers.Count; $index += 2) {
            $x = [double]$numbers[$index] / $ViewBox * $Canvas
            $y = [double]$numbers[$index + 1] / $ViewBox * $Canvas
            $points.Add([System.Drawing.PointF]::new([float]$x, [float]$y))
        }

        $polygons.Add($points)
    }

    if ($polygons.Count -eq 0) { throw 'The source SVG has no polygon subpaths.' }
    return $polygons
}

function Get-GlyphPath {
    param($Polygons, [double]$Scale, [double]$Offset)

    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.FillMode = [System.Drawing.Drawing2D.FillMode]::Winding
    foreach ($polygon in $Polygons) {
        $points = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
        foreach ($point in $polygon) {
            $x = [double]$Offset + [double]$point.X * $Scale
            $y = [double]$Offset + [double]$point.Y * $Scale
            $points.Add([System.Drawing.PointF]::new([float]$x, [float]$y))
        }
        $path.AddPolygon($points.ToArray())
    }
    return $path
}

function Get-RoundedTilePath {
    param([double]$Size, [double]$CornerRadius)

    $diameter = $CornerRadius * 2.0
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc(0, 0, [float]$diameter, [float]$diameter, 180, 90)
    $path.AddArc([float]($Size - $diameter), 0, [float]$diameter, [float]$diameter, 270, 90)
    $path.AddArc([float]($Size - $diameter), [float]($Size - $diameter), [float]$diameter, [float]$diameter, 0, 90)
    $path.AddArc(0, [float]($Size - $diameter), [float]$diameter, [float]$diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconBitmap {
    param([int]$Size, $Polygons, [System.Drawing.Color]$TileColour, [System.Drawing.Color]$GlyphColour)

    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.Clear([System.Drawing.Color]::Transparent)

        $corner = [math]::Max(1.0, [double]$Size * $Radius)
        $tile = Get-RoundedTilePath -Size $Size -CornerRadius $corner
        $brush = [System.Drawing.SolidBrush]::new($TileColour)
        try { $graphics.FillPath($brush, $tile) } finally { $brush.Dispose(); $tile.Dispose() }

        # The glyph is centred in the tile, inset by the padding.
        $inner = [double]$Size * (1.0 - 2.0 * $Padding)
        $scale = $inner / $Canvas
        $offset = ([double]$Size - $inner) / 2.0
        $glyph = Get-GlyphPath -Polygons $Polygons -Scale $scale -Offset $offset
        $glyphBrush = [System.Drawing.SolidBrush]::new($GlyphColour)
        try { $graphics.FillPath($glyphBrush, $glyph) } finally { $glyphBrush.Dispose(); $glyph.Dispose() }
    } finally {
        $graphics.Dispose()
    }
    return $bitmap
}

function Write-IcoFile {
    param([string]$Target, [int[]]$IconSizes, $Polygons, [System.Drawing.Color]$TileColour, [System.Drawing.Color]$GlyphColour)

    $images = [System.Collections.Generic.List[object]]::new()
    foreach ($size in $IconSizes) {
        $bitmap = New-IconBitmap -Size $size -Polygons $Polygons -TileColour $TileColour -GlyphColour $GlyphColour
        $stream = [System.IO.MemoryStream]::new()
        try {
            # PNG frames carry the alpha the rounded tile needs at every size.
            # DIB frames were tried as well, to mirror the shipped icon, but the
            # resource compiler rejects 32-bpp DIB frames written by hand
            # (rc RC2176 "old DIB"), and an 8-bpp palette would lose the
            # antialiased corners.
            $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            $images.Add([pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() })
        } finally {
            $stream.Dispose()
            $bitmap.Dispose()
        }
    }

    # ICONDIR, one ICONDIRENTRY per image, then the images.
    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$images.Count)

        $offset = 6 + 16 * $images.Count
        foreach ($image in $images) {
            $dimension = if ($image.Size -ge 256) { 0 } else { $image.Size }
            $writer.Write([byte]$dimension)
            $writer.Write([byte]$dimension)
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$image.Bytes.Length)
            $writer.Write([uint32]$offset)
            $offset += $image.Bytes.Length
        }

        foreach ($image in $images) { $writer.Write($image.Bytes) }
        $writer.Flush()
        [System.IO.File]::WriteAllBytes($Target, $stream.ToArray())
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Write-TileSvgFile {
    param([string]$Target, [string]$PathData, [string]$TileColour, [string]$GlyphColour)

    $inset = $Padding * $Canvas
    $scale = 1.0 - 2.0 * $Padding
    $corner = $Radius * $Canvas

    $text = @"
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1536 1536">
  <!--
      The application icon as vector artwork: a rounded grey tile carrying the
      same glyph as AudioPlaybackConnector.Rounded.svg, in white. Generated by
      tools/gen-icons.ps1 from that file - do not edit by hand.
  -->
  <rect width="1536" height="1536" rx="$([math]::Round($corner, 1))" fill="$TileColour" />
  <g transform="translate($([math]::Round($inset, 1)) $([math]::Round($inset, 1))) scale($([math]::Round($scale, 6)))">
    <path d="$PathData" fill="$GlyphColour" />
  </g>
</svg>
"@
    Set-Content -LiteralPath $Target -Value $text -Encoding UTF8 -NoNewline
}

$svg = Get-Content -LiteralPath $Source -Raw
$viewBoxMatch = [regex]::Match($svg, 'viewBox="([^"]+)"')
if (-not $viewBoxMatch.Success) { throw 'The source SVG has no viewBox.' }
$viewBox = [double](($viewBoxMatch.Groups[1].Value -split '[\s,]+')[2])

$pathData = Get-SvgPathData -Svg $svg
$polygons = Get-SvgPolygons -PathData $pathData -ViewBox $viewBox

Write-Host "glyph subpaths: $($polygons.Count) (viewBox $viewBox)"

$tileColour = [System.Drawing.ColorTranslator]::FromHtml($TileColor)
$glyphColour = [System.Drawing.ColorTranslator]::FromHtml($GlyphColor)

Write-IcoFile -Target $IcoPath -IconSizes $Sizes -Polygons $polygons -TileColour $tileColour -GlyphColour $glyphColour
Write-TileSvgFile -Target $TilePath -PathData $pathData -TileColour $TileColor -GlyphColour $GlyphColor

Write-Host ("wrote {0} ({1} bytes, sizes {2})" -f (Split-Path -Leaf $IcoPath), (Get-Item -LiteralPath $IcoPath).Length, ($Sizes -join '/'))
Write-Host ("wrote {0} ({1} bytes)" -f (Split-Path -Leaf $TilePath), (Get-Item -LiteralPath $TilePath).Length)
