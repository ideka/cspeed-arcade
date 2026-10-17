# Captures the game window with PrintWindow (works when the window is covered or off-screen).
#   screenshot.ps1 <output.png> [process name]
param([string]$out, [string]$proc = "CaliforniaSpeedArcade")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$p = Get-Process $proc -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$r = New-Object Win+RECT
[void][Win]::GetClientRect($p.MainWindowHandle, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.Right)), ([Math]::Max(1, $r.Bottom))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][Win]::PrintWindow($p.MainWindowHandle, $hdc, 3) # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
