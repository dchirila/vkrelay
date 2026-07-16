# vkrelay2 dev helper: turn a worker raw pixel dump into a PNG.
#
# Two worker dev hooks write these dumps:
#   * VKRELAY2_DEBUG_DUMP_READBACK=<path>: copy_image_to_buffer dest buffers ->
#     "<path>.<n>.raw" + "<path>.<n>.dims" ("<w> <h>"). Assumed BGRA8 (the legacy format).
#   * VKRELAY2_DEBUG_DUMP_PRESENT=<path>: the pre-present swapchain capture -> the same
#     "<path>.<n>.raw" + "<path>.<n>.dims", PLUS a self-describing "<path>.<n>.meta" sidecar
#     (key=value: format, format_name, order=BGRA|RGBA|UNKNOWN, width, height, bpp, usage).
#
# This converts ONE .raw into a viewable PNG. It prefers the .meta sidecar (so it honors the actual
# VkFormat's byte order); if only the legacy .dims is present it assumes BGRA8. It FAILS CLEARLY on a
# format whose byte order it does not know (order=UNKNOWN), rather than mis-decoding.
#
# Usage:
#   raw_to_png.ps1 -Raw C:\tmp\rb.0.raw [-Out C:\tmp\rb.0.png]
param(
    [Parameter(Mandatory = $true)][string]$Raw,
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Raw)) { Write-Output "RESULT status=no_raw ($Raw)"; exit 2 }

$meta = [System.IO.Path]::ChangeExtension($Raw, ".meta")
$dims = [System.IO.Path]::ChangeExtension($Raw, ".dims")

$w = 0; $h = 0; $order = "BGRA"; $fmtName = "(legacy-dims:assumed-BGRA8)"
if (Test-Path $meta) {
    # The self-describing sidecar (key=value per line); honors the real VkFormat byte order.
    $kv = @{}
    foreach ($line in Get-Content $meta) {
        if ($line -match '^\s*([A-Za-z_]+)\s*=\s*(.+?)\s*$') { $kv[$Matches[1]] = $Matches[2] }
    }
    if ($kv.ContainsKey('width')) { $w = [int]$kv['width'] }
    if ($kv.ContainsKey('height')) { $h = [int]$kv['height'] }
    if ($kv.ContainsKey('order')) { $order = $kv['order'] }
    if ($kv.ContainsKey('format_name')) { $fmtName = $kv['format_name'] }
}
elseif (Test-Path $dims) {
    # Legacy artifacts predate the .meta sidecar (e.g. readback dumps): assume BGRA8.
    $wh = (Get-Content $dims -Raw).Trim() -split '\s+'
    $w = [int]$wh[0]; $h = [int]$wh[1]
    $order = "BGRA"
}
else {
    Write-Output "RESULT status=no_meta_or_dims ($meta / $dims)"; exit 2
}

if ($w -le 0 -or $h -le 0) { Write-Output "RESULT status=bad_dims (${w}x${h})"; exit 3 }
if (-not $Out) { $Out = [System.IO.Path]::ChangeExtension($Raw, ".png") }

if ($order -ne "BGRA" -and $order -ne "RGBA") {
    # Fail clearly rather than guess: the converter only knows 4-byte BGRA/RGBA byte orders.
    Write-Output ("RESULT status=unsupported_format order=$order format=$fmtName " +
        "(raw_to_png.ps1 decodes only 4-byte BGRA/RGBA; no PNG written)")
    exit 4
}

$bytes = [System.IO.File]::ReadAllBytes($Raw)
if ($bytes.Length -lt ($w * $h * 4)) {
    Write-Output ("RESULT status=short_raw (have " + $bytes.Length + ", need " + ($w * $h * 4) + ")")
    exit 3
}

if ($order -eq "RGBA") {
    # System.Drawing's Format32bppArgb is BGRA in memory, so RGBA bytes need R<->B swapped. A per-byte
    # PowerShell loop over a ~15 MB buffer is far too slow; do it in compiled code at native speed.
    if (-not ([System.Management.Automation.PSTypeName]'VkrPx').Type) {
        Add-Type -TypeDefinition 'public static class VkrPx { public static void SwapRB(byte[] b){ for(int i=0;i+2<b.Length;i+=4){ byte t=b[i]; b[i]=b[i+2]; b[i+2]=t; } } }'
    }
    [VkrPx]::SwapRB($bytes)
}

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
# After any swap, the buffer is BGRA in memory == Format32bppArgb -> direct copy. (For 32bpp the
# stride has no row padding, so a single Copy is exact.)
[System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, ($w * $h * 4))
$bmp.UnlockBits($data)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)

# A rough "did it render anything?" signal (sparse grid).
$colors = New-Object System.Collections.Generic.HashSet[int]
for ($y = 0; $y -lt $h; $y += 8) { for ($x = 0; $x -lt $w; $x += 8) { [void]$colors.Add($bmp.GetPixel($x, $y).ToArgb()) } }
Write-Output ("RESULT status=ok size=${w}x${h} order=$order format=$fmtName unique_colors=" + $colors.Count + " png=$Out")
$bmp.Dispose()
exit 0
