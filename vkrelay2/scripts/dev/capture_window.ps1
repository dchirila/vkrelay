# vkrelay2 focused window-capture dev helper.
#
# Finds a vkrelay2 worker window (class prefix vkrelay2_worker_win_) and saves a PNG of its CLIENT
# area + an optional JSON metadata sidecar -- for eyeballing placeholder chrome / popups / z-order /
# focus during bring-up.
#
# IMPORTANT -- this is a DEVELOPER CONVENIENCE, NOT a gate. The trustworthy, occlusion/minimize-proof
# capture path is the worker's OWN source layers (vkrelay2-capture + DebugCaptureWindow). This
# helper scrapes the desktop, so its limits are real and documented:
#   - PrintWindow returns black/solid frames for flip-model Vulkan (DXGI) surface content;
#   - the CopyFromScreen fallback (-AllowForegroundFallback) needs the window visible + unoccluded;
#   - minimized / off-screen windows are unreliable with either method.
# It works well for placeholder/chrome windows (where PrintWindow is fine) and quick visual checks.
#
# EnumWindows callback discipline (the original vkrelay capture.ps1's documented gotcha): anything
# Write-Output'd -- or written through a closure-captured local -- from inside the delegate is LOST
# (pipeline scope mismatch). Collect via a $global ArrayList instead.
#
# Output: one "RESULT status=... [unique_colors=N] [client=WxH] ..." line on stdout.
# Exit codes: 0 captured ok, 2 no matching window / bad args, 3 capture failed / degenerate.
#
# Usage:
#   capture_window.ps1 -OutputPng out.png [-EmitJson out.json]
#       [-Hwnd <n> | -ProcessId <n> | -TitleMatch <substr>] [-ClassPrefix vkrelay2_worker_win_]
#       [-AllowForegroundFallback] [-MinUniqueColors <n>] [-MaxWaitSec 10] [-WarmupSec 1]
#   (-TitleMatch pairs with the worker's debug title tag: run the worker with
#    VKRELAY2_DEBUG_WINDOW_TITLES=1 so titles read "vkrelay2 [xid=0x...]".)
param(
    [string]$OutputPng = "",
    [string]$EmitJson = "",
    [Int64]$Hwnd = 0,
    [int]$ProcessId = 0,
    [string]$TitleMatch = "",
    [string]$ClassPrefix = "vkrelay2_worker_win_",
    [switch]$AllowForegroundFallback,
    [int]$MinUniqueColors = 0,
    [int]$MaxWaitSec = 10,
    [int]$WarmupSec = 1
)
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Vkr2Cap {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public const uint PW_CLIENTONLY = 0x1;
    public const uint PW_RENDERFULLCONTENT = 0x2;
    public const int SW_SHOWNOACTIVATE = 4;
}
"@

# Per-monitor-v2 DPI awareness BEFORE any GetClientRect / PrintWindow (-4). Without it, scaled
# monitors virtualize client sizes and PrintWindow yields cropped/black-banded bitmaps.
[void][Vkr2Cap]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

if (-not $OutputPng) { Write-Output "RESULT status=bad_args (need -OutputPng)"; exit 2 }

function Find-Vkr2Windows {
    $global:vkr2_found = New-Object System.Collections.ArrayList
    $cb = [Vkr2Cap+EnumProc] {
        param($h, $l)
        if (-not [Vkr2Cap]::IsWindowVisible($h)) { return $true }
        $c = New-Object System.Text.StringBuilder 256
        [Vkr2Cap]::GetClassNameW($h, $c, $c.Capacity) | Out-Null
        if (-not $c.ToString().StartsWith($script:ClassPrefix)) { return $true }
        # Skip zero-sized HWNDs (dead leftovers a teardown has not reaped yet).
        $r = New-Object Vkr2Cap+RECT
        [Vkr2Cap]::GetClientRect($h, [ref]$r) | Out-Null
        if (($r.Right - $r.Left) -le 0 -or ($r.Bottom - $r.Top) -le 0) { return $true }
        $t = New-Object System.Text.StringBuilder 512
        [Vkr2Cap]::GetWindowTextW($h, $t, $t.Capacity) | Out-Null
        $owner = [uint32]0
        [Vkr2Cap]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
        [void]$global:vkr2_found.Add([pscustomobject]@{
                Hwnd = $h; Class = $c.ToString(); Title = $t.ToString(); Pid = [int]$owner
            })
        return $true
    }
    [Vkr2Cap]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $global:vkr2_found
}

function Select-Target($rows) {
    if ($Hwnd -ne 0) { return ($rows | Where-Object { [Int64]$_.Hwnd -eq $Hwnd } | Select-Object -First 1) }
    if ($ProcessId -ne 0) { return ($rows | Where-Object { $_.Pid -eq $ProcessId } | Select-Object -First 1) }
    if ($TitleMatch) { return ($rows | Where-Object { $_.Title -match [regex]::Escape($TitleMatch) } | Select-Object -First 1) }
    return ($rows | Select-Object -First 1)
}

function Measure-UniqueColors($bmp, $w, $h) {
    # Sparse grid (every 4th pixel) -- a rough "did it render anything?" signal, fast on large frames.
    $colors = New-Object System.Collections.Generic.HashSet[int]
    for ($y = 0; $y -lt $h; $y += 4) {
        for ($x = 0; $x -lt $w; $x += 4) { [void]$colors.Add($bmp.GetPixel($x, $y).ToArgb()) }
    }
    return $colors.Count
}

# Poll for a matching window (try at least once even when -MaxWaitSec is 0).
$deadline = (Get-Date).AddSeconds($MaxWaitSec)
$picked = $null
do {
    $picked = Select-Target (@(Find-Vkr2Windows))
    if ($picked) { break }
    if ((Get-Date) -ge $deadline) { break }
    Start-Sleep -Milliseconds 250
} while ($true)
if (-not $picked) { Write-Output "RESULT status=hwnd_not_found class_prefix=$ClassPrefix"; exit 2 }

if ($WarmupSec -gt 0) { Start-Sleep -Seconds $WarmupSec }

$hwnd = [IntPtr]$picked.Hwnd
$client = New-Object Vkr2Cap+RECT
$window = New-Object Vkr2Cap+RECT
[Vkr2Cap]::GetClientRect($hwnd, [ref]$client) | Out-Null
[Vkr2Cap]::GetWindowRect($hwnd, [ref]$window) | Out-Null
$cw = $client.Right - $client.Left
$ch = $client.Bottom - $client.Top
$ww = $window.Right - $window.Left
$wh = $window.Bottom - $window.Top
$hasChrome = ($wh -gt $ch) -or ($ww -gt $cw)
if ($cw -le 0 -or $ch -le 0) { Write-Output "RESULT status=zero_client title=`"$($picked.Title)`""; exit 3 }

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $cw, $ch
$method = "printwindow"
$fallback = $false
try {
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $hdc = $gfx.GetHdc()
        try {
            $flags = [Vkr2Cap]::PW_CLIENTONLY -bor [Vkr2Cap]::PW_RENDERFULLCONTENT
            if (-not [Vkr2Cap]::PrintWindow($hwnd, $hdc, $flags)) {
                Write-Output "RESULT status=printwindow_failed client=${cw}x${ch}"; exit 3
            }
        } finally { $gfx.ReleaseHdc($hdc) }
    } finally { $gfx.Dispose() }

    $unique = Measure-UniqueColors $bmp $cw $ch

    # PrintWindow can return a solid frame for flip-model (Vulkan/DXGI) content. With explicit
    # opt-in, fall back to copying the client area FROM THE SCREEN (needs the window visible).
    if ($unique -le 2 -and $AllowForegroundFallback) {
        [void][Vkr2Cap]::ShowWindow($hwnd, [Vkr2Cap]::SW_SHOWNOACTIVATE)
        [void][Vkr2Cap]::BringWindowToTop($hwnd)
        [void][Vkr2Cap]::SetForegroundWindow($hwnd)
        Start-Sleep -Milliseconds 300
        $origin = New-Object Vkr2Cap+POINT
        if ([Vkr2Cap]::ClientToScreen($hwnd, [ref]$origin)) {
            $g2 = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g2.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size($cw, $ch)))
            } finally { $g2.Dispose() }
            $method = "copyfromscreen"; $fallback = $true
            $unique = Measure-UniqueColors $bmp $cw $ch
        }
    }

    $dir = Split-Path -Parent $OutputPng
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save($OutputPng, [System.Drawing.Imaging.ImageFormat]::Png)

    if ($EmitJson) {
        $meta = [ordered]@{
            hwnd          = [Int64]$picked.Hwnd
            class         = $picked.Class
            title         = $picked.Title
            pid           = $picked.Pid
            client        = "${cw}x${ch}"
            window        = "${ww}x${wh}"
            chrome        = $hasChrome
            dpi_awareness = "per-monitor-v2"
            method        = $method
            fallback_used = $fallback
            unique_colors = $unique
            png           = $OutputPng
        }
        $jdir = Split-Path -Parent $EmitJson
        if ($jdir -and -not (Test-Path $jdir)) { New-Item -ItemType Directory -Force -Path $jdir | Out-Null }
        ($meta | ConvertTo-Json -Compress) | Set-Content -Path $EmitJson -Encoding utf8
    }

    $degenerate = ($MinUniqueColors -gt 0 -and $unique -lt $MinUniqueColors)
    $status = if ($degenerate) { "degenerate" } else { "ok" }
    Write-Output ("RESULT status={0} unique_colors={1} client={2}x{3} chrome={4} method={5} fallback={6} title=`"{7}`" png={8}" -f `
            $status, $unique, $cw, $ch, $hasChrome, $method, $fallback, $picked.Title, $OutputPng)
    if ($degenerate) { exit 3 }
} finally { $bmp.Dispose() }
exit 0
