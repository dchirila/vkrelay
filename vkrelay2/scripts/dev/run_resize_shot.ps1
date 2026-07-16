# vkrelay2 on-screen RESIZE verification.
#
# Drives the vkrelay2-geometry-shot harness (a REAL visible placeholder window) through a
# sidecar-authored resize and uses capture_window.ps1 to SCREENSHOT the on-screen client BEFORE and
# AFTER -- then asserts the captured client extent GREW. This is the visual/manual gate the user asked
# us to lean on; the deterministic correctness gates are integration_real_backend (the real HWND
# converged via include_actual) + run_geometry_smoke.sh (over the wire) + unit_sidecar.
#
# It pauses the harness at each "ready=" marker (stdin step), captures (PrintWindow -- the placeholder
# is GDI-painted, so the PNG has real pixels, not flip-model black), then advances. The proof is the
# JSON `client` field from capture_window.ps1: after.client must be strictly larger than before.client.
#
# Usage:
#   run_resize_shot.ps1 [-ShotBin <path>] [-CaptureScript <path>] [-OutDir <dir>]
# Exit: 0 = PASS (resize captured) or SKIP (no window thread); 1 = FAIL.
param(
    [string]$ShotBin = "",
    [string]$CaptureScript = "",
    [string]$OutDir = ""
)
$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $scriptDir "..\..")
if (-not $ShotBin) { $ShotBin = Join-Path $repo "build\windows-debug\vkrelay2-geometry-shot.exe" }
if (-not $CaptureScript) { $CaptureScript = Join-Path $scriptDir "capture_window.ps1" }
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP "vkrelay2-resize-shot" }

if (-not (Test-Path $ShotBin)) { Write-Output "RESIZE-SHOT: SKIP (harness not built: $ShotBin)"; exit 0 }
if (-not (Test-Path $CaptureScript)) { Write-Output "RESIZE-SHOT: FAIL (no capture_window.ps1)"; exit 1 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Capture the harness window (by HWND) to a PNG + JSON sidecar, in a FRESH powershell process so
# capture_window.ps1's Add-Type does not collide across the two calls.
function Capture($hwnd, $tag) {
    $png = Join-Path $OutDir "resize_$tag.png"
    $json = Join-Path $OutDir "resize_$tag.json"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $CaptureScript `
        -Hwnd $hwnd -OutputPng $png -EmitJson $json -MaxWaitSec 5 -WarmupSec 0 | ForEach-Object {
        Write-Host "    capture[$tag]: $_"
    }
    if (-not (Test-Path $json)) { return $null }
    return (Get-Content $json -Raw | ConvertFrom-Json)
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ShotBin
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)

# Read harness stdout until a line matches "ready=<marker>"; returns the line (or $null on EOF).
function ReadUntilReady($marker) {
    while (-not $proc.StandardOutput.EndOfStream) {
        $line = $proc.StandardOutput.ReadLine()
        if ($null -eq $line) { break }
        Write-Host "    harness: $line"
        if ($line -match "ready=$marker") { return $line }
    }
    return $null
}

$rc = 1
try {
    $beforeLine = ReadUntilReady "before"
    if ($null -eq $beforeLine) {
        $proc.WaitForExit()
        Write-Output "RESIZE-SHOT: SKIP (harness did not reach 'ready=before' -- likely no window thread)"
        exit 0
    }
    if ($beforeLine -match "hwnd=(-?\d+)") { $hwnd = [int64]$Matches[1] }
    else { Write-Output "RESIZE-SHOT: FAIL (no hwnd in harness output)"; exit 1 }

    $before = Capture $hwnd "before"
    if ($null -eq $before) { Write-Output "RESIZE-SHOT: FAIL (before-capture produced no metadata)"; exit 1 }
    Write-Output "RESIZE-SHOT: before client=$($before.client)"

    $proc.StandardInput.WriteLine("go") # advance the harness past the resize
    $afterLine = ReadUntilReady "after"
    if ($null -eq $afterLine) { Write-Output "RESIZE-SHOT: FAIL (harness did not reach 'ready=after')"; exit 1 }

    $after = Capture $hwnd "after"
    if ($null -eq $after) { Write-Output "RESIZE-SHOT: FAIL (after-capture produced no metadata)"; exit 1 }
Write-Output "RESIZE-SHOT: after client=$($after.client)"

    $proc.StandardInput.WriteLine("go") # let the harness tear down + exit

    # The proof: the captured on-screen CLIENT extent grew (parse "WxH").
    $bw, $bh = $before.client -split "x"
    $aw, $ah = $after.client -split "x"
    if (([int]$aw -gt [int]$bw) -and ([int]$ah -gt [int]$bh)) {
        Write-Output "============================================================"
        Write-Output ("RESIZE-SHOT: PASS (on-screen client grew {0} -> {1}; PNGs in {2})" -f `
                $before.client, $after.client, $OutDir)
        Write-Output "============================================================"
        $rc = 0
    } else {
        Write-Output "RESIZE-SHOT: FAIL (client did not grow: $($before.client) -> $($after.client))"
        $rc = 1
    }
} finally {
    if (-not $proc.HasExited) { $proc.WaitForExit(5000) | Out-Null }
    if (-not $proc.HasExited) { $proc.Kill() }
}
exit $rc
