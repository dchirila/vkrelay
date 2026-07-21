# Troubleshooting

The launcher reports every bring-up phase and preserves logs on failure. Start with the last phase
marker and the diagnostic bundle path printed at exit.

## Diagnostic Bundles

On a nonzero exit, the launcher preserves available session files under:

```text
/tmp/vkrelay2-logs-<launcher-pid>/
```

The bundle can contain:

| File | Contents |
|---|---|
| `phase.log` | Ordered launcher milestones and elapsed time. |
| `app.log` | Target stdout/stderr, including Mesa and loader errors. |
| `worker.log` | Auto-started supervisor/worker stderr. |
| `sidecar.log` | X11 WM, capture, input, and geometry bridge output. |
| `weston.log` | Private headless compositor output. |
| `xwayland.log` | Private X server output. |

Set `VKRELAY2_LOG_DIR` to choose another destination. A failure before the private display starts
still preserves `phase.log` when possible.

## No Build Artifact Found

Typical messages mention a missing `vkrelay2-launch`, ICD manifest, sidecar, or Windows build.

Build release artifacts from WSL:

```bash
cd vkrelay2
./scripts/dev/rebuild_all.sh --release
```

If only debug artifacts exist, the launcher can use them. If both exist, release wins. A warning that
release is older than debug means the application run would otherwise use stale release binaries;
rebuild with `--release`.

## No Healthy Daemon Reachable

First verify the Windows binaries exist and that the worker was built with the real Vulkan backend.
Then run:

```bash
./linux/launcher/vkrun --list-gpus
```

For mirrored WSL networking, the daemon and WSL client share `127.0.0.1`. For NAT mode:

```bash
VKRELAY2_BIND=0.0.0.0 \
    ./linux/launcher/vkrun --list-gpus
```

Allow the supervisor through Windows Firewall only on the intended network profiles.

## Daemon Auto-Start Fails: Windows Interop Broken

If the launcher reports that it cannot execute Windows binaries from WSL (or a manual
`powershell.exe` in WSL fails with `cannot execute binary file: Exec format error`), the
WSL interop state is broken — every Windows `.exe` launch from WSL fails, so the daemon cannot be
auto-started and the Windows build lane cannot run.

Close the affected distribution, then restart WSL completely from Windows PowerShell or Command
Prompt:

```powershell
wsl --shutdown
```

Reopen the distribution and retry. The launcher and `rebuild_all.sh` preflight actual Windows
executable launch and print this remedy before attempting further Windows work.

## --list-gpus Refuses To Print An Adapter List

`--list-gpus` never prints the mocked fallback list (a fake NVIDIA T1200 / Intel UHD /
AMD Radeon 610M) as if it were hardware. It fails with the cause instead, in two situations:

- no daemon was reachable — the message names the endpoint it tried; fix daemon reachability
  first (see above);
- the daemon is up but stamps its adapter list as mocked, because its worker has no real Vulkan
  enumeration — typically a Windows build without the Vulkan SDK (see Real Worker Required
  below).

A printed list therefore contains only adapters that exist on the Windows host, with real driver
names and LUIDs. Setting `VKRELAY2_ALLOW_MOCK_GPUS=1` restores the mock output for tests and
offline tooling; a daemon predating the provenance stamp prints as before, where any `(mock)`
driver name still identifies fake entries.

## Daemon Appears Wedged

The launcher distinguishes an open TCP port from a daemon that completes the control handshake. If
it reports that the daemon appears wedged, stop it on Windows and retry:

```powershell
Stop-Process -Name vkrelay2-supervisor -Force -ErrorAction SilentlyContinue
Stop-Process -Name vkrelay2-worker -Force -ErrorAction SilentlyContinue
```

Do not start a second supervisor on the same port. The bind will fail and can obscure the original
problem.

## Real Worker Required

If the launcher says the daemon launches `mock` workers, the Windows build did not include the real
Vulkan backend or a test daemon is still running. Install the Vulkan SDK, reconfigure the Windows
build, stop the existing supervisor, and rebuild.

The Windows CMake configure output should contain:

```text
vkrelay2: Vulkan SDK found; worker gets the real backend (--vulkan-backend real)
```

If it says `no Vulkan SDK` even though the SDK is installed and you are building from WSL via
`rebuild_all.sh`: WSL's interop service caches the Windows environment, so a machine-level
`VULKAN_SDK` set after WSL started is invisible to interop-launched builds until the next full
Windows reboot. Pass it through explicitly instead (see Building, Windows Dependencies).

## Private Display Does Not Start

The normal WSLg configuration mounts `/tmp/.X11-unix` read-only. vkrelay2 detects that condition and
uses an unprivileged user/mount namespace with a private writable overlay. If unprivileged user
namespaces are disabled, the launcher prints two manual remedies.

Check that these commands are available:

```bash
command -v unshare weston Xwayland
```

Also inspect `weston.log` and `xwayland.log` in the failure bundle. The launcher tries X displays
`:30` through `:60` and never removes another server's lock.

## Application Reports That It Is Running as root

The private X11 workaround briefly acquires mount privilege in an outer user namespace, then enters
a nested namespace that restores the invoking numeric user and primary group before contacting the
daemon or starting the application. Verify the application-facing identity with:

```bash
./linux/launcher/vkrun -- bash -lc 'id; getent passwd "$(id -u)"'
```

If the launcher cannot prove that the nested namespace preserves identity and retains a writable
private `/tmp/.X11-unix`, it fails closed and prints the same manual remount/WSLg-disable remedies as
the private-display error above. Upgrade an older vkrelay2 package if the command instead reports
uid 0 for an ordinary user. Supplementary groups may appear as the user-namespace overflow group;
the invoking numeric uid and primary gid are the enforced compatibility boundary.

## Xwayland Crashes During Pointer Warps or Window Moves

A crash during Blender view rotation (a pointer recenter) or a window move usually means the stock
Ubuntu Xwayland was selected in the seat-less private compositor instead of the private guarded
build — it NULL-dereferences on those seatless paths on all supported releases (22.04, 24.04, 26.04).

Build the private compatible server:

```bash
sudo apt update
VKRELAY2_XWL_INSTALL_DEPS=1 \
    bash src_ext/xwayland/build_private_xwayland.sh
```

On the next launch, look for:

```text
vkrelay2: auto-selected compatible private Xwayland stage
```

The launcher rejects a staged binary whose provenance, package version, architecture, build ID, or
freshness does not match the current host. See `src_ext/xwayland/UPSTREAM.md` before updating that
baseline.

## OpenGL Uses the Wrong Driver or Shows a Blank Window

The default and `--frontend opengl46zink` paths export Mesa's Zink selection. Confirm Zink exists:

```bash
test -f /usr/lib/x86_64-linux-gnu/dri/zink_dri.so
```

Install `libgl1-mesa-dri` if it is missing. Do not select `--frontend vulkan13` for an OpenGL
application; that mode intentionally does not steer GL through Zink.

Inspect `app.log` for `MESA`, `ZINK`, Vulkan loader, or `VK_ERROR_*` messages. Also check that the
generated manifest points at the shared library in the same build tree.

## Application Reports VK_ERROR_FEATURE_NOT_PRESENT or VK_ERROR_EXTENSION_NOT_PRESENT

The relay exposes the intersection of host support and implemented relay support. A Windows driver
supporting an extension is not sufficient if its structures or commands are not forwarded by the
relay. Applications should query capabilities and select a supported path.

Use the native frontend for native Vulkan 1.3 applications:

```bash
./linux/launcher/vkrun --frontend vulkan13 -- app
```

The default Zink-safe lane intentionally reports a narrower Vulkan surface.

## Device Lost During Resize or Presentation

Collect the full failure bundle and keep the application command, selected GPU, window operation,
and monitor placement used to reproduce it. The most useful files are `app.log`, `worker.log`,
`sidecar.log`, and `phase.log`.

For targeted traces during development:

```bash
VKRELAY2_ICD_TRACE=1 \
    ./linux/launcher/vkrun -- app
```

`VKRELAY2_GEOM_TRACE` is consumed by the Windows worker, not by the WSL launcher or ICD. To use it,
stop any reused daemon and start the Windows supervisor from an environment that already contains
`VKRELAY2_GEOM_TRACE=1`; prefixing the WSL launcher command does not configure an existing or
auto-started Windows daemon. See [Runtime Tracing](development.md#runtime-tracing).

Traces are verbose and can affect timing. Use them to localize a reproducible issue, not for normal
launches.

## Window Coordinates Are Wrong on Multiple Monitors

vkrelay2 snapshots the full Windows virtual desktop once per graphical session and exposes it as one
physical-pixel guest canvas. Moving, resizing, maximizing, restoring, input, and popups are expected
to work across negative-origin, mixed-resolution/DPI, and portrait arrangements. The guest still
sees one giant output: pixel dimensions do not automatically scale at a DPI boundary, so a window's
apparent physical size can change, and fullscreen covers the full canvas.

If Windows display topology or a taskbar work area changed after launch, look in the worker log for
`display configuration changed` and `restart this vkrelay2 graphical session`. The active session
deliberately keeps its original transform rather than partly adopting the new layout. Close and
relaunch the application to capture a new snapshot. If the desktop did not change, preserve the log
bundle and include the snapshot ID, virtual bounds, monitor rectangles, and the affected window's
guest/host client rectangle in a bug report.

## Non-Rectangular X11 Windows Have a Black Background

Legacy X11 applications such as `xeyes`, `oclock`, and `xclock` can use the binary XShape bounding
region rather than an alpha channel. vkrelay2 masks captured pixels outside that region so stale or
undefined backing-store content is never shown after a resize. Placeholder HWNDs are still opaque,
however, so the defined outside-shape area appears black instead of revealing the Windows desktop as
it does under WSLg. Desktop transparency for shaped placeholder windows is not currently supported.
