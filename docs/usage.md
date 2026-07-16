# Usage

Use `vkrelay2/linux/launcher/vkrun` for normal application launches. The lower-level
`vkrelay2-launch` executable creates launch descriptors and opens control sessions, but by itself it
does not create the private display, pin the ICD, start the sidecar, or own teardown.

Commands below assume the current directory is the implementation directory, `vkrelay2/`.

## List GPUs

```bash
./linux/launcher/vkrun --list-gpus
```

The list comes from Windows Vulkan enumeration through the running real-backend daemon. Each entry
includes its stable vendor/device identifiers, LUID, Vulkan version, driver, type, usability, and
default role.

## Run an Application

The basic form is:

```bash
./linux/launcher/vkrun [session options] -- application [arguments...]
```

The `--` boundary is significant. Everything after it is preserved as the application's argument
vector, including whitespace and empty arguments.

Examples:

```bash
# OpenGL through Mesa Zink, using automatic GPU selection
./linux/launcher/vkrun --gpu auto -- glxgears

# OpenSCAD with a file argument
./linux/launcher/vkrun --gpu high-performance -- \
    openscad /usr/share/openscad/examples/Basics/CSG.scad

# Blender through the default OpenGL/Zink frontend
./linux/launcher/vkrun --gpu high-performance -- blender

# Native Vulkan with the Vulkan 1.3-capable lane
./linux/launcher/vkrun \
    --frontend vulkan13 --gpu high-performance -- vkcube
```

The launcher returns the target application's exit code.

## Graphics Frontends

Select a frontend with `--frontend`:

| Value | Behavior |
|---|---|
| `auto` | Default. Enables Mesa Zink for OpenGL and selects the Zink-safe Vulkan capability lane. |
| `opengl46zink` | Explicit spelling of the OpenGL-through-Zink path; currently equivalent to `auto`. |
| `vulkan13` | Native Vulkan lane. Does not configure the system OpenGL stack to use Zink. |

Use `vulkan13` for native Vulkan applications that need the relay's Vulkan 1.3 surface. Do not use it
for an OpenGL application unless that application is intentionally meant to use a non-relay system
OpenGL implementation.

The native lane reports Vulkan 1.3 only when both the selected host device and the worker's
relay-capability audit allow it. Applications must still query every optional extension and feature.
See [Vulkan Support Model](vulkan-support.md) for the implemented families and current structural
limits.

## GPU Selection

`--gpu` accepts:

| Selector | Meaning |
|---|---|
| `auto` | Prefer the adapter marked high-performance, then the first usable adapter. |
| `high-performance` | Select the default high-performance or first usable discrete adapter. |
| `integrated` | Select a usable integrated adapter. |
| `vendor:0x10de` | Select by PCI vendor ID. |
| `device:0x1002:0x164e` | Select by PCI vendor and device IDs. |
| `luid:<value>` | Select by the Windows adapter LUID printed by `--list-gpus`. |
| `name:<substring>` | Case-insensitive adapter-name match. |
| `index:<n>` | Select the current list index. This is convenient but unstable across hardware/driver changes. |

For saved scripts, prefer LUID or vendor/device selectors over `index:<n>`.

## Working Directory and Environment

Session options before `--` are interpreted by `vkrelay2-launch`:

```bash
./linux/launcher/vkrun \
    --cwd /home/user/project \
    --env APP_MODE=development \
    --env LOG_LEVEL=debug \
    -- ./bin/my-app --project "example project"
```

For byte-exact automation, the launch helper also accepts NUL-separated `--argv-file`, `--env-file`,
and `--cwd-file` inputs, as well as versioned JSON descriptors. Run:

```bash
./linux/launcher/vkrun --help
```

for the complete descriptor-oriented CLI. These modes are primarily useful for integration tooling;
the ordinary `-- application ...` form is preferred for interactive use.

## Display Selection

The launch descriptor accepts `--display auto|x11|wayland`. The supported projected-window path is
currently X11 through the launcher's private rootless Xwayland server. Leave the value at `auto` for
normal use. `wayland` is not a complete native-Wayland presentation path and should not be used as
one.

## Daemon and Networking

The Windows supervisor listens on TCP port `13579` by default. The launcher:

- reuses a daemon only after a successful protocol handshake;
- starts a fresh real-backend daemon through `powershell.exe` when none is listening;
- refuses to launch when a process accepts the port but the control protocol is unresponsive.

Mirrored WSL networking is recommended. In `%UserProfile%\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored
```

After changing WSL configuration, run `wsl --shutdown` from Windows.

For older NAT-mode WSL, the Windows daemon must bind beyond loopback:

```bash
VKRELAY2_BIND=0.0.0.0 \
    ./linux/launcher/vkrun --list-gpus
```

The launcher then tries the WSL default gateway. Windows Firewall may prompt the first time. Binding
to `0.0.0.0` exposes the daemon to networks permitted by firewall policy; use mirrored networking
when possible.

To use another port consistently:

```bash
VKRELAY2_DAEMON_PORT=24680 \
    ./linux/launcher/vkrun --list-gpus
```

## Runtime Overrides

Normal runs should require no environment overrides. The supported diagnostic and deployment
overrides are:

| Variable | Purpose |
|---|---|
| `VKRELAY2_DAEMON_HOST` | Explicit supervisor host; skips automatic host selection. |
| `VKRELAY2_DAEMON_PORT` | Control-plane port; default `13579`. |
| `VKRELAY2_BIND` | Bind address used when auto-starting the Windows daemon. |
| `VKRELAY2_WIN_BUILD` | Directory containing `vkrelay2-supervisor.exe` and `vkrelay2-worker.exe`. |
| `VKRELAY2_LAUNCH` | Explicit Linux `vkrelay2-launch` executable. |
| `VKRELAY2_ICD_MANIFEST` | Explicit generated ICD manifest. |
| `VKRELAY2_SIDECAR_BIN` | Explicit sidecar executable. |
| `VKRELAY2_XWAYLAND_BIN` | Explicit Xwayland executable; bypasses automatic private-build selection. |
| `VKRELAY2_WORKER_LOG` | Worker/supervisor stderr file used by an auto-started daemon. |
| `VKRELAY2_LOG_DIR` | Bundle directory instead of `/tmp/vkrelay2-logs-<pid>`; setting it also preserves logs after a successful run. |
| `VKRELAY2_GUEST_ROOT` | Explicit private X root size as `WIDTHxHEIGHT`; diagnostic/power-user override for the normally snapshotted virtual-desktop extent. |
| `VKRELAY2_ALLOW_DEFAULT_GUEST_ROOT=1` | Continue with Weston's default root if the host-display query fails. This can make parts of windows unclickable and is intended only for diagnosis. |
| `VKRELAY2_CONTROL_TIMEOUT_MS` | Client control read deadline; default 15000 ms. |
| `VKRELAY2_SERVE_IDLE_TIMEOUT_MS` | Supervisor accepted-session idle deadline; default 30000 ms. |
| `VKRELAY2_OPEN_SESSION_TIMEOUT` | Shell backstop for worker-session creation; default 60 seconds. |

Variables such as `VKRELAY2_DATA_PLANE_*`, `VKRELAY2_SIDECAR_PLANE_*`, tokens,
`VKRELAY2_HOST_WORK_W`, `VKRELAY2_HOST_WORK_H`, `VKRELAY2_HOST_DPI`, and
`VKRELAY2_NATIVE_LANE` are launcher-to-component contracts. Do not set them manually.

## Current Limitations

The following are properties of the current implementation, not configuration errors:

- The projected-window path supports X11/Xwayland applications. Native Wayland window projection is
  not implemented.
- The guest display is one physical-pixel canvas spanning the snapshotted Windows virtual desktop.
  Negative host origins, mixed resolution/DPI, portrait monitors, per-monitor placement/work areas,
  and cross-monitor input/popups are supported, but guest RandR/Xinerama still reports one giant
  output with one global guest DPI. A window retains its pixel extent across a DPI boundary, so its
  apparent physical size can change; the relay does not perform automatic logical-size scaling.
- Fullscreen in this single-canvas level is root-wide, not per-monitor. Guest-visible per-monitor
  outputs, DPI/scale, toolkit monitor selection, and per-monitor fullscreen require the deferred
  multi-output level.
- The display layout and work areas are immutable for a graphical session. A monitor hotplug,
  resolution/orientation change, or taskbar work-area change produces a restart-required worker
  diagnostic while keeping the old mapping stable. Restart the application session to adopt the
  new desktop; live transactional topology updates are not implemented.
- One selected physical device and one relay-visible queue are exposed to an application session.
- The Vulkan surface is deliberately bounded. Unsupported extensions and features are omitted or
  rejected, and unsupported structure/command shapes fail rather than being approximated.
- A Vulkan 1.3 API version on the native lane describes the implemented required feature matrix; it
  is not a Khronos conformance claim. Full CTS coverage is not complete.
- OpenGL support is the behavior Mesa Zink can build on the relay's Vulkan surface. It is not a
  separate GLX command-forwarding implementation.
- Ubuntu 22.04, 24.04, and 26.04 amd64 are validated for session bring-up, `--list-gpus`, the
  native Vulkan lane, and OpenGL through Zink (glxgears, OpenSCAD, Blender run on all three). The
  launcher adapts the Zink capability lane to the guest Mesa version automatically: Mesa 23.x
  (Ubuntu 22.04) uses the Zink-safe Vulkan 1.2 surface; Mesa 24+ (Ubuntu 24.04/26.04) requires a
  Vulkan 1.3 device, which the launcher opens for the GL lane on those distributions.
- A seatless weston-headless stack triggers two upstream Xwayland NULL-dereference crashes — a
  client pointer warp, and (on Xwayland 24.1.x) a window-move damage report — so vkrelay2 runs a
  private guarded Xwayland instead of the stock server. `src_ext/xwayland/build_private_xwayland.sh`
  builds it for the host distribution automatically on Ubuntu 22.04, 24.04, and 26.04 amd64 (it
  builds from that distribution's own security-current Xwayland source and applies only the
  applicable guard patch(es)), and the launcher auto-selects the result. Other distributions need a
  compatible system Xwayland or their own security-current baseline.
- There is no OS-level installer, service registration, or stable configuration file. A supported
  binary tar package is available (see [Installing](install.md)); a source checkout runs from
  build-tree paths.
