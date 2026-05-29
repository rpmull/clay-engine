param(
    [Parameter(Mandatory = $true)]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

function New-ResizedSquareBitmap {
    param(
        [Parameter(Mandatory = $true)]
        [System.Drawing.Image]$Image,

        [Parameter(Mandatory = $true)]
        [int]$Size
    )

    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    if ($Image.HorizontalResolution -gt 0 -and $Image.VerticalResolution -gt 0) {
        $bitmap.SetResolution($Image.HorizontalResolution, $Image.VerticalResolution)
    }

    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

        $scale = [Math]::Min($Size / [double]$Image.Width, $Size / [double]$Image.Height)
        $drawWidth = [Math]::Max(1, [int][Math]::Round($Image.Width * $scale))
        $drawHeight = [Math]::Max(1, [int][Math]::Round($Image.Height * $scale))
        $offsetX = [int][Math]::Floor(($Size - $drawWidth) / 2.0)
        $offsetY = [int][Math]::Floor(($Size - $drawHeight) / 2.0)

        $destinationRect = [System.Drawing.Rectangle]::new($offsetX, $offsetY, $drawWidth, $drawHeight)
        $graphics.DrawImage($Image, $destinationRect)
    }
    finally {
        $graphics.Dispose()
    }

    return $bitmap
}

function Convert-BitmapToPngBytes {
    param(
        [Parameter(Mandatory = $true)]
        [System.Drawing.Bitmap]$Bitmap
    )

    $memoryStream = [System.IO.MemoryStream]::new()
    try {
        # Icon.Save() quantizes frames to 4bpp here, which breaks alpha on the
        # small taskbar/titlebar sizes and produces black square backgrounds.
        $Bitmap.Save($memoryStream, [System.Drawing.Imaging.ImageFormat]::Png)
        return [byte[]]$memoryStream.ToArray()
    }
    finally {
        $memoryStream.Dispose()
    }
}

$resolvedSource = [System.IO.Path]::GetFullPath($Source)
$resolvedDestination = [System.IO.Path]::GetFullPath($Destination)

if (-not (Test-Path -LiteralPath $resolvedSource -PathType Leaf)) {
    throw "Source PNG not found: $resolvedSource"
}

$destinationDirectory = Split-Path -Path $resolvedDestination -Parent
if ($destinationDirectory) {
    New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
}

$sourceImage = [System.Drawing.Image]::FromFile($resolvedSource)
try {
    $iconEntries = [System.Collections.Generic.List[object]]::new()
    foreach ($size in @(16, 32, 48, 64, 128, 256)) {
        $bitmap = New-ResizedSquareBitmap -Image $sourceImage -Size $size
        try {
            $iconImageBytes = Convert-BitmapToPngBytes -Bitmap $bitmap
            $iconEntries.Add([PSCustomObject]@{
                Size = $size
                Width = if ($size -ge 256) { [byte]0 } else { [byte]$size }
                Height = if ($size -ge 256) { [byte]0 } else { [byte]$size }
                ColorCount = [byte]0
                Reserved = [byte]0
                Planes = [UInt16]1
                BitCount = [UInt16]32
                Bytes = $iconImageBytes
            })
        }
        finally {
            $bitmap.Dispose()
        }
    }

    $fileStream = [System.IO.File]::Open($resolvedDestination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $writer = [System.IO.BinaryWriter]::new($fileStream)
    try {
        $writer.Write([UInt16]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]$iconEntries.Count)

        $offset = 6 + ($iconEntries.Count * 16)
        foreach ($entry in $iconEntries) {
            $writer.Write([byte]$entry.Width)
            $writer.Write([byte]$entry.Height)
            $writer.Write([byte]$entry.ColorCount)
            $writer.Write([byte]$entry.Reserved)
            $writer.Write([UInt16]$entry.Planes)
            $writer.Write([UInt16]$entry.BitCount)
            $writer.Write([UInt32]$entry.Bytes.Length)
            $writer.Write([UInt32]$offset)

            $offset += $entry.Bytes.Length
        }

        foreach ($entry in $iconEntries) {
            $writer.Write([byte[]]$entry.Bytes)
        }
    }
    finally {
        $writer.Dispose()
    }
}
finally {
    $sourceImage.Dispose()
}
