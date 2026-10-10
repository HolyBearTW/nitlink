# Deterministic high-resolution vector/text composition using the original icon.
# The 3x sources are downsampled by MUI without distorting their aspect ratio.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$icon = [Drawing.Icon]::new((Join-Path $PSScriptRoot '../nitlink.ico'), 128, 128)
function Write-BrandImage([string]$Name, [int]$Width, [int]$Height, [bool]$Sidebar, [int]$Scale) {
    $bitmap = [Drawing.Bitmap]::new($Width * $Scale, $Height * $Scale, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = 'AntiAlias'
    $graphics.TextRenderingHint = 'AntiAliasGridFit'
    $graphics.ScaleTransform($Scale, $Scale)
    $graphics.Clear([Drawing.Color]::FromArgb(16,24,39))
    $accent = [Drawing.Pen]::new([Drawing.Color]::FromArgb(48,201,238), 2)
    $ink = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(237,242,250))
    $muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(147,166,196))
    $large = [Drawing.Font]::new('Segoe UI', $(if($Sidebar){19}else{14}), [Drawing.FontStyle]::Bold)
    $small = [Drawing.Font]::new('Segoe UI', $(if($Sidebar){10}else{8.5}))
    try {
        if ($Sidebar) {
            $graphics.DrawIcon($icon, [Drawing.Rectangle]::new(27,32,110,110))
            $graphics.DrawString('NitLink', $large, $ink, 28,158)
            $graphics.DrawLine($accent, 30,198,134,198)
            $graphics.DrawString("HolyBear`nEdition", $small, $muted, 30,211)
            $graphics.DrawString('WINDOWS x64', $small, $muted, 30,280)
        } else {
            $graphics.DrawIcon($icon, [Drawing.Rectangle]::new(5,7,42,42))
            $graphics.DrawString('NitLink', $large, $ink, 53,5)
            $graphics.DrawString('HolyBear Edition', $small, $muted, 53,31)
        }
        $bitmap.Save((Join-Path $PSScriptRoot $Name), [Drawing.Imaging.ImageFormat]::Bmp)
    } finally {
        $small.Dispose(); $large.Dispose(); $muted.Dispose(); $ink.Dispose(); $accent.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
    }
}
try {
    Write-BrandImage 'wizard.bmp' 164 314 $true 1
    Write-BrandImage 'wizard@3x.bmp' 164 314 $true 3
    Write-BrandImage 'banner.bmp' 150 57 $false 1
    Write-BrandImage 'banner@3x.bmp' 150 57 $false 3
}
finally { $icon.Dispose() }
