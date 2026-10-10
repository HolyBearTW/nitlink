# Deterministic vector/text composition using the original NitLink icon.
# These BMP sources are checked in; neither Setup nor CI needs a graphics tool.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$icon = [Drawing.Icon]::new((Join-Path $PSScriptRoot '../nitlink.ico'), 128, 128)
function Write-BrandImage([string]$Name, [int]$Width, [int]$Height, [bool]$Sidebar) {
    $bitmap = [Drawing.Bitmap]::new($Width, $Height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = 'AntiAlias'
    $graphics.TextRenderingHint = 'AntiAliasGridFit'
    $graphics.Clear([Drawing.Color]::FromArgb(16,24,39))
    $accent = [Drawing.Pen]::new([Drawing.Color]::FromArgb(48,201,238), 2)
    $ink = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(237,242,250))
    $muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(147,166,196))
    $large = [Drawing.Font]::new('Segoe UI', $(if($Sidebar){20}else{14}), [Drawing.FontStyle]::Bold)
    $small = [Drawing.Font]::new('Segoe UI', 9)
    try {
        if ($Sidebar) {
            $graphics.DrawIcon($icon, [Drawing.Rectangle]::new(27,35,110,110))
            $graphics.DrawString('NitLink', $large, $ink, 28,164)
            $graphics.DrawLine($accent, 30,205,134,205)
            $graphics.DrawString("HolyBear`nEdition", $small, $muted, 30,220)
            $graphics.DrawString('WINDOWS x64', $small, $muted, 30,282)
        } else {
            $graphics.DrawIcon($icon, [Drawing.Rectangle]::new(5,6,44,44))
            $graphics.DrawString('NitLink', $large, $ink, 52,7)
            $graphics.DrawString('HolyBear Edition', $small, $muted, 52,31)
        }
        $bitmap.Save((Join-Path $PSScriptRoot $Name), [Drawing.Imaging.ImageFormat]::Bmp)
    } finally {
        $small.Dispose(); $large.Dispose(); $muted.Dispose(); $ink.Dispose(); $accent.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
    }
}
try { Write-BrandImage 'wizard.bmp' 164 314 $true; Write-BrandImage 'banner.bmp' 180 57 $false }
finally { $icon.Dispose() }
