# Capture a window by title (used to verify the Steam Link UI on the capture card preview)
param(
    [string]$TitleLike = "Capture View",
    [string]$Out = "$env:TEMP\capture.png",
    [int]$MaxWidth = 1280
)

Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Win32Capture {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@

$proc = Get-Process | Where-Object { $_.MainWindowTitle -like "*$TitleLike*" } | Select-Object -First 1
if (-not $proc) {
    Write-Error "window not found: $TitleLike"
    exit 1
}

$hwnd = $proc.MainWindowHandle
if ([Win32Capture]::IsIconic($hwnd)) {
    [Win32Capture]::ShowWindow($hwnd, 9) | Out-Null
    Start-Sleep -Milliseconds 300
}
[Win32Capture]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 200

$rect = New-Object Win32Capture+RECT
if (-not [Win32Capture]::GetWindowRect($hwnd, [ref]$rect)) {
    Write-Error "GetWindowRect failed"
    exit 1
}

$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) {
    Write-Error "invalid window size ${width}x${height}"
    exit 1
}

$bmp = New-Object System.Drawing.Bitmap($width, $height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
$g.Dispose()

if ($width -gt $MaxWidth) {
    $scaled = New-Object System.Drawing.Bitmap($MaxWidth, [int]($height * $MaxWidth / $width))
    $g2 = [System.Drawing.Graphics]::FromImage($scaled)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.DrawImage($bmp, 0, 0, $scaled.Width, $scaled.Height)
    $g2.Dispose()
    $bmp.Dispose()
    $bmp = $scaled
}

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$size = "$($bmp.Width)x$($bmp.Height)"
$bmp.Dispose()

Write-Output "$Out $size"
