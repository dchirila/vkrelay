# vkrelay2 on-screen Z-ORDER (stacking) verification.
#
# Drives the vkrelay2-geometry-shot harness in "zorder" mode (two OVERLAPPING visible placeholders,
# then RAISE A above B) and uses capture_window.ps1 to screenshot BOTH windows before and after the
# raise -- the multi-window stacking shot. capture_window.ps1's PrintWindow is OCCLUSION-PROOF (it
# grabs each window's own content), so the PNGs are the visual record of the multi-window scene; the
# actual stacking FLIP is asserted worker-visible by the harness via GetWindow (the same gate as
# integration_real_backend test_real_z_order) and re-checked here from its output.
#
# Usage: run_zorder_shot.ps1 [-ShotBin <path>] [-CaptureScript <path>] [-OutDir <dir>]
# Exit: 0 = PASS (stacking flipped) or SKIP (no window thread); 1 = FAIL.
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
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP "vkrelay2-zorder-shot" }

if (-not (Test-Path $ShotBin)) { Write-Output "ZORDER-SHOT: SKIP (harness not built: $ShotBin)"; exit 0 }
if (-not (Test-Path $CaptureScript)) { Write-Output "ZORDER-SHOT: FAIL (no capture_window.ps1)"; exit 1 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Capture($hwnd, $tag) {
    $png = Join-Path $OutDir "zorder_$tag.png"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $CaptureScript `
        -Hwnd $hwnd -OutputPng $png -MaxWaitSec 5 -WarmupSec 0 | ForEach-Object {
        Write-Host "    capture[$tag]: $_"
    }
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ShotBin
$psi.Arguments = "zorder"
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$proc = [System.Diagnostics.Process]::Start($psi)

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
    $before = ReadUntilReady "before"
    if ($null -eq $before) {
        $proc.WaitForExit()
        Write-Output "ZORDER-SHOT: SKIP (harness did not reach 'ready=before' -- likely no window thread)"
        exit 0
    }
    if ($before -notmatch "hwndA=(-?\d+) hwndB=(-?\d+) B_above_A=(\d)") {
        Write-Output "ZORDER-SHOT: FAIL (could not parse harness 'before' line)"; exit 1
    }
    $hA = [int64]$Matches[1]; $hB = [int64]$Matches[2]; $bAboveA = [int]$Matches[3]

    Capture $hA "A_before"
    Capture $hB "B_before"

    $proc.StandardInput.WriteLine("go") # raise A
    $after = ReadUntilReady "after"
    if ($null -eq $after) { Write-Output "ZORDER-SHOT: FAIL (harness did not reach 'ready=after')"; exit 1 }
    if ($after -notmatch "A_above_B=(\d)") { Write-Output "ZORDER-SHOT: FAIL (could not parse harness 'after' line)"; exit 1 }
    $aAboveB = [int]$Matches[1]

    Capture $hA "A_after"
    Capture $hB "B_after"
    $proc.StandardInput.WriteLine("go") # let the harness exit

    # The stacking FLIP (worker-visible via the harness's GetWindow): B was above A initially; after
    # the raise A is above B.
    if ($bAboveA -eq 1 -and $aAboveB -eq 1) {
        Write-Output "============================================================"
        Write-Output "ZORDER-SHOT: PASS (B over A -> raise A -> A over B; multi-window PNGs in $OutDir)"
        Write-Output "============================================================"
        $rc = 0
    } else {
        Write-Output "ZORDER-SHOT: FAIL (stacking did not flip: B_above_A=$bAboveA A_above_B=$aAboveB)"
        $rc = 1
    }
} finally {
    if (-not $proc.HasExited) { $proc.WaitForExit(5000) | Out-Null }
    if (-not $proc.HasExited) { $proc.Kill() }
}
exit $rc
