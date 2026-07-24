# Testing

vkrelay2 spans a Linux graphics stack, a private compositor/X server, two relay protocols, a
Windows Vulkan implementation, and Win32 presentation. No single test layer is sufficient, so the
repository uses progressively broader oracles.

## Test layers

| Layer | What it proves | Normal entry point |
|---|---|---|
| Unit and decoder fuzz | Wire bounds, admission, ownership, bookkeeping, and pure policy | `ctest` / `rebuild_all.sh --all` |
| Mock integration | Control/data-plane and lifecycle behavior without a GPU | `ctest` |
| Real-backend integration | Vulkan execution and Win32 behavior directly on the host backend | `ctest` |
| Deterministic canaries | Exact pixels, buffer values, feature exposure, and launcher/ICD honesty through the full stack | `linux/launcher/run_*_smoke.sh` |
| Real-application catalog | Final composed HWND topology and non-degenerate pixels from common Vulkan and GL/Zink applications | `linux/launcher/render_catalog.sh` |
| Aggregate smoke gate | The maintained deterministic, real-app, window lifecycle/input, and synchronization subset | `linux/launcher/gate.sh` |

The capture catalog complements the deterministic canaries; it does not replace them. A color-count
threshold detects blank or solid final presentation, while the canaries retain stronger exact-value
or feature-specific assertions. Real-app logs are also scanned for relay/Mesa rejection and crash
signatures because an application can display frames after an earlier command failed.

## Build and compiled tests

From Ubuntu under WSL:

```bash
cd vkrelay2
./scripts/dev/rebuild_all.sh --all
```

This builds both platforms, runs the Debug CTest presets, verifies C/C++ formatting, and parses every
tracked shell script. Release binaries are separate:

```bash
./scripts/dev/rebuild_all.sh --release
```

The launcher prefers release artifacts when they exist. Rebuild release before application testing
if the helper reports that release is older than Debug.

## Real-application render catalog

The catalog runs these rows sequentially through the production `vkrun` path:

- upstream `vkcube`;
- `glxgears` through Mesa Zink;
- a bounded `glmark2` build scene through Mesa Zink; and
- the packaged OpenSCAD example in its normal GUI.

Each installed application must produce exactly one visible worker top-level with native chrome and
meet its calibrated sparse-grid unique-color threshold. Captures, topology JSON, app output, and
session logs are retained under the printed artifact directory and scanned together for fatal
relay, Mesa, worker, and application signatures.

```bash
# Missing external applications are reported as skips.
bash linux/launcher/render_catalog.sh build/linux-release

# Require every catalog dependency (release/pre-publish mode).
bash linux/launcher/render_catalog.sh build/linux-release --strict

# Run and retain artifacts for one row.
bash linux/launcher/render_catalog.sh build/linux-release --case glxgears
```

`VKRELAY2_RENDER_CATALOG_DIR` selects a persistent artifact directory. The current thresholds use
the helper's every-fourth-pixel grid and are structural, not golden-image comparisons; recalibrate
them when the capture algorithm changes.

## Aggregate smoke gate

The gate composes existing runners for exact GL readback, tessellation/geometry, MRT, multi-frame
transitions, bounded real GL applications, the render catalog, window lifecycle/input/resize/
iconify/popup behavior, and the native synchronization2 triple-lane canary.

```bash
# Fail fast; missing catalog apps are skips.
bash linux/launcher/gate.sh build/linux-release

# Require catalog dependencies and collect every stage result.
bash linux/launcher/gate.sh build/linux-release --strict --keep-going

# Build first, then run the strict gate.
./scripts/dev/rebuild_all.sh --release --smoke-gate
```

Set `VKRELAY2_SMOKE_GATE_DIR` to retain the aggregate logs at a chosen path and
`VKRELAY2_SMOKE_GPU` to override the default `integrated` selector for the frame-transition stage.
`VKRELAY2_SMOKE_STAGE_TIMEOUT` overrides the normal per-stage 360-second budget, while
`VKRELAY2_CATALOG_STAGE_TIMEOUT` overrides the catalog's 900-second multi-row budget.
The gate is opt-in: the normal build remains usable on machines without a display or host GPU.

## Final HWND capture

`scripts/dev/capture_window.ps1` can capture one worker window or report all matching topology:

```powershell
.\scripts\dev\capture_window.ps1 `
  -OutputPng out.png -EmitJson out.json `
  -ExpectedWindowCount 1 -MinUniqueColors 40 `
  -AllowForegroundFallback

.\scripts\dev\capture_window.ps1 `
  -ListWindows -EmitJson topology.json
```

Candidates are ordered deterministically. A capture without an HWND/PID/title selector fails as
ambiguous when more than one worker top-level matches. `PrintWindow(PW_RENDERFULLCONTENT)` is tried
first; an explicitly allowed `CopyFromScreen` fallback handles drivers whose flip-model content is
solid through `PrintWindow`. Both paths are retried within explicit bounds, and the tool emits
exactly one `RESULT` line.

The screen fallback requires the target to be visible and unoccluded. Use the worker source-layer
capture described in [Development](development.md#focused-window-capture) when testing chrome or
cursor content independently of desktop composition.
