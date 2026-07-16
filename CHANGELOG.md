# Changelog

All notable changes to vkrelay2 are documented here. The project follows
[Semantic Versioning](https://semver.org/). While the version is below 1.0, behavior may change
between minor releases.

## Unreleased

### Build

- `scripts/dev/rebuild_all.sh` no longer requires Visual Studio Community: it auto-detects the
  installed edition (Community, Professional, Enterprise, or BuildTools) and honors a
  `VKRELAY2_VS_DIR` override for non-standard install roots.

## 0.1.0 — 2026-07-16

Initial public release.

### Graphics

- Vulkan ICD that forwards an application's Vulkan work to a Windows host Vulkan driver over a
  control/data plane (JSON handshake, then a binary RPC envelope).
- Opt-in Vulkan 1.3 lane (dynamic rendering, synchronization2, buffer device address, and other
  1.3 families) gated on the selected host device and the relay's own capability.
- OpenGL applications supported through Mesa Zink (OpenGL translated to Vulkan) into the same ICD.
- Faithful-or-fail-closed semantics: the ICD exposes only functionality the relay actually backs,
  and rejects unsupported extensions, features, pipeline shapes, and commands rather than emulating
  them. It is not presented as a Vulkan conformant implementation.

### Windowing

- A native Win32 top-level window for each application window, plus popup/menu windows, input,
  cursors, move/resize feedback, minimize/restore, and captured X11 chrome.
- Multi-monitor support across a single physical-pixel canvas: mixed resolution/DPI,
  negative-origin, and portrait Windows monitor layouts, with snapshot-driven per-monitor placement
  and maximize behavior.

### Sessions and tooling

- A per-application isolated Weston + private Xwayland session; the launcher establishes the
  graphics and window-management paths before it starts the application.
- A long-lived Windows supervisor that owns per-application workers; the host Vulkan adapter is
  selected at launch time. `--list-gpus` reports only real host adapters and refuses to present a
  mocked fallback as hardware.
- A binary package build (`scripts/dev/package_release.sh`) that installs and runs without build
  tools.

### Platforms

- Windows 11 with WSL2 (Ubuntu 22.04 jammy, 24.04 noble, and 26.04 resolute; amd64). The Windows
  supervisor and worker build with MSVC; the Linux ICD, launcher, and sidecar build with GCC. The
  full dual-platform gate and the reproducible hash-pinned private-Xwayland baseline run on 22.04.
