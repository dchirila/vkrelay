# Changelog

All notable changes to vkrelay2 are documented here. The project follows
[Semantic Versioning](https://semver.org/). While the version is below 1.0, behavior may change
between minor releases.

## Unreleased

### Build

- `scripts/dev/rebuild_all.sh` no longer requires Visual Studio Community: it auto-detects the
  installed edition (Community, Professional, Enterprise, or BuildTools) and honors a
  `VKRELAY2_VS_DIR` override for non-standard install roots.
- `scripts/dev/rebuild_all.sh` now preflights the complete Linux and Windows build dependencies
  before touching build trees, reports actionable Ubuntu/Visual Studio/Vulkan SDK installation
  guidance, and supports a preflight-only `--check-deps` mode.
- Clarified that the guarded private Xwayland is an application-run and release-package dependency,
  added a matching Linux readiness check to `rebuild_all.sh`, and documented when its separate build
  stage must be refreshed. Direct builds, `--windows-only`, `--list-gpus`, normal `ctest` runs, and
  binary-package installation do not build the stage. Preflight-only success now also returns status
  zero when no temporary Windows wrapper was created.
- Fixed `VKRELAY2_XWL_INSTALL_DEPS=1` on Ubuntu 22.04 by resolving the extracted source package's
  `debian/control` through `mk-build-deps`; Jammy APT rejects the previously used local control path.
- Isolated private-Xwayland package tests from WSLg's host GPU stack by selecting Mesa `llvmpipe` on
  WSL, and fixed strict-gate reporting so full Meson test names remain intact.
- Binary releases are distro-specific `.deb` packages with stable GitHub asset names and SHA-256
  checksum files. They install Linux components under `/usr`, materialize verified Windows
  executables under the invoking user's `%LOCALAPPDATA%` on first launch, and remove that Windows
  payload when APT uninstalls or upgrades the package.

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
