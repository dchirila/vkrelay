# Building

vkrelay2 has two build products with different toolchains:

- the Linux/WSL build contains the launcher, Vulkan ICD, sidecar, capture tool, and canaries;
- the Windows build contains the supervisor, worker, real Vulkan backend, and integration tests.

Both are generated from `vkrelay2/CMakeLists.txt` with Ninja and CMake presets.

## Supported Development Configuration

The configuration the full dual-platform gate runs on is:

- Windows 11 and WSL2;
- Ubuntu 22.04 amd64 in WSL;
- Visual Studio 2026 (Community, Professional, or Enterprise) with the x64 C++ toolchain;
- the LunarG Vulkan SDK visible to CMake on Windows;
- a Windows Vulkan 1.3 driver for the selected GPU.

Ubuntu **22.04 (jammy)**, **24.04 (noble)**, and **26.04 (resolute)** amd64 are all validated for
session bring-up, `--list-gpus`, the native Vulkan lane, and OpenGL through Zink. The private-Xwayland
recipe builds for each of them (see [Private Xwayland](#private-xwayland)); the checked-in gate that
produces the reproducible hash-pinned baseline runs on 22.04. Other distributions can build the normal
Linux targets, but need a compatible system Xwayland or their own security-current baseline.

## WSL Dependencies

`rebuild_all.sh` preflights the compiler, build tools, and development libraries before it changes
either build tree. On Ubuntu, install the complete build set with:

```bash
sudo apt update
sudo apt install \
    build-essential cmake ninja-build pkg-config git \
    libvulkan-dev \
    libxcb1-dev libxcb-composite0-dev libxcb-shape0-dev libxcb-xtest0-dev libxcb-xfixes0-dev \
    libgl-dev libx11-dev
```

Install the runtime, diagnostic, and private-Xwayland recipe dependencies with:

```bash
sudo apt update
sudo apt install \
    vulkan-tools libgl1-mesa-dri mesa-utils \
    weston xwayland quilt
```

`quilt` is used by the private-Xwayland build recipe (see below), which is required for the normal
application path on all validated distributions: the launcher refuses the distro's stock `xwayland`
package as known-crashing, so `apt`'s `xwayland` alone is not sufficient to run applications.

The important runtime capabilities are:

- `weston` with the headless backend;
- `Xwayland` and X11 socket support (the launcher runs the private build, see below);
- Mesa's `zink_dri.so` for OpenGL applications (in `libgl1-mesa-dri`; the launcher warns if absent);
- XComposite for guest chrome capture;
- XShape for bounding-region masking of non-rectangular X11 windows;
- XTest for input injection;
- XFixes for guest cursor capture.

Two host-level capabilities are easy to lose and produce confusing failures; the launcher now
preflights both when it has to auto-start the daemon:

- **Windows interop must actually execute `.exe` files.** If the `WSLInterop` binfmt registration
  is missing (this can flake on systemd-enabled distributions), every Windows binary fails with
  `Exec format error` and Windows builds or daemon auto-start cannot run. Both `rebuild_all.sh` and
  the launcher probe actual `.exe` execution and recommend a full `wsl --shutdown` restart.
- **WSL networking must let the client reach the daemon.** Mirrored networking
  (`[wsl2] networkingMode=mirrored` in `%UserProfile%\.wslconfig`) is recommended; in NAT mode the
  daemon's default `127.0.0.1` bind is unreachable from WSL and `VKRELAY2_BIND=0.0.0.0` is
  required. The launcher warns when it detects the dead combination.

CMake treats the XCB extension libraries as optional so a partial build can still compile, but the
corresponding runtime behavior is then absent. Install all packages above for the normal application
path.

## Windows Dependencies

The complete Windows build lane requires:

1. Visual Studio 2026 (Community, Professional, Enterprise, or Build Tools). In Visual Studio
   Installer, select the **Desktop development with C++** workload and ensure these individual
   components are selected:
   - **MSVC Build Tools for x64/x86 (Latest)**;
   - **C++ CMake tools for Windows** (provides the CMake and Ninja used by the wrapper);
   - **Windows 11 SDK**;
   - **C++ Clang tools for Windows**, required only for `--lint` and `--all` because the lint lane
     uses its `clang-format.exe`.

   ATL, MFC, C++/CLI, AddressSanitizer, vcpkg, ARM toolchains, and older MSVC toolsets are not
   required. These component names and IDs come from Microsoft's
   [Visual Studio 2026 workload manifest](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-professional?view=visualstudio#desktop-development-with-c).
2. The current LunarG **Vulkan SDK for Windows x64**, with **Core only**. On the installer's
   **Select Components** screen, click **None**: **The Vulkan SDK Core (Always Installed)** remains
   installed automatically. Leave GLM Headers, SDL libraries and headers, Volk, Shader Toolchain
   Debug Symbols, Vulkan Memory Allocator, and ARM64 cross-compilation binaries unchecked. vkrelay2
   requires only `Include\vulkan\vulkan.h` and `Lib\vulkan-1.lib`. The installer normally uses
   `C:\VulkanSDK\<version>` and sets `VULKAN_SDK`; see LunarG's
   [Windows SDK installation guide](https://vulkan.lunarg.com/doc/view/latest/windows/getting_started.html#install-the-sdk).
3. A current Vulkan driver from the GPU vendor. This is a runtime requirement, not part of the SDK;
   LunarG explicitly notes that the SDK does not install a driver.

The CMake project can still produce a deliberately reduced mock-only worker without the SDK, but
`rebuild_all.sh` is the complete product gate and therefore treats a missing SDK as an error. It
checks both required SDK files before configuring CMake, preventing a partial build from appearing
green.

Note when building through `scripts/dev/rebuild_all.sh` from WSL right after installing the SDK:
WSL's interop service caches the Windows environment, so a freshly set machine-level `VULKAN_SDK`
is not visible to interop-launched builds until the next full Windows reboot (a `wsl --shutdown`
is not enough). Until then, pass it through explicitly:

```bash
export VULKAN_SDK='C:\VulkanSDK\<version>'
export WSLENV=${WSLENV:+$WSLENV:}VULKAN_SDK/w
./scripts/dev/rebuild_all.sh --release --windows-only --clean
```

The Windows configure output states which backend the worker got; after installing the SDK it must
say `Vulkan SDK found; worker gets the real backend`, not `no Vulkan SDK`.

## Build Both Halves From WSL

From the implementation directory:

```bash
cd vkrelay2
./scripts/dev/rebuild_all.sh --release
```

The script builds Linux natively in WSL and invokes the Visual Studio toolchain through Windows
interop. Before cleaning or configuring, it reports all missing Linux build dependencies and checks
the selected Visual Studio environment, Windows SDK, and Vulkan SDK. To run only that preflight:

```bash
./scripts/dev/rebuild_all.sh --check-deps
```

The script auto-detects the installed edition (Community, Professional, Enterprise, or
BuildTools, in that order) under:

```text
C:\Program Files\Microsoft Visual Studio\18\<Edition>
```

For a non-standard install location, point it at the install root explicitly:

```bash
export VKRELAY2_VS_DIR='D:\VS\18\Professional'
```

Debug builds are the default:

```bash
./scripts/dev/rebuild_all.sh
```

Useful modes:

```bash
./scripts/dev/rebuild_all.sh --all          # debug build, tests, and lint
./scripts/dev/rebuild_all.sh --clean --all  # clean full gate
./scripts/dev/rebuild_all.sh --linux-only
./scripts/dev/rebuild_all.sh --windows-only
```

The launcher prefers release artifacts when both release and debug trees exist. It reports the
selected flavor and warns if a release artifact is older than its debug sibling.

## Manual CMake Builds

Run these commands from `vkrelay2/`, not from the repository root.

### Linux / WSL

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Release:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

Sanitizers:

```bash
cmake --preset linux-asan
cmake --build --preset linux-asan
ctest --preset linux-asan
```

### Windows

In an x64 Native Tools command prompt:

```bat
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Release:

```bat
cmake --preset windows-release
cmake --build --preset windows-release
```

## Private Xwayland

The private Weston headless session has no Wayland seat because vkrelay2 carries input over its own
sidecar path. The stock Xwayland build crashes in that seat-less configuration — a client pointer
warp on all supported versions, and (on Xwayland 24.1.x) a window-move damage report. Blender view
rotation is a common trigger.

`vkrelay2/src_ext/xwayland/` contains a reproducible build recipe based on Ubuntu's complete
security-patched Xwayland source package plus the applicable local null-seat guard patch(es). It does
not replace the system executable. The recipe is distro-adaptive across **22.04 (jammy)**, **24.04
(noble)**, and **26.04 (resolute)** amd64; it detects the host and derives which guard patches apply
from the Xwayland upstream version (the pointer-warp guard on all versions; the damage-report guard
from 24.1).

Refresh the apt package index, then build it once on the host you are packaging for:

```bash
sudo apt update
VKRELAY2_XWL_INSTALL_DEPS=1 \
    bash src_ext/xwayland/build_private_xwayland.sh
```

The script uses one of two acquisition models, recorded in the stage's `PROVENANCE.txt`:

- **jammy (`acquire: hashpin`)** — verifies all source inputs against checked-in SHA-256 hashes,
  verifies that the pinned source revision is still the current Ubuntu security candidate, and applies
  a strict CVE-count and package-test gate. This is the reproducible baseline.
- **noble / resolute (`acquire: aptsrc`)** — acquires the source with `apt-get source` (integrity from
  the distribution's signed apt keyring, always the current candidate — freshness by construction). The
  jammy-tuned package-test allowlist does not generalize, so off-jammy the suite is run and recorded
  under a hard timeout rather than fatally gated; off-jammy correctness rests on the distribution source
  + the guards + a resolved-config parity check + the application actually rendering.

Both models build with Ubuntu's packaging rules and hardening flags, and write
`build/src_ext/xwayland/stage/Xwayland` and `PROVENANCE.txt` atomically. The provenance records a
`release_ready:` verdict that the packager enforces (see [Installing From a Binary Package](install.md)).

On a compatible host, the launcher automatically prefers the staged binary after matching the
provenance `target: <codename>/<arch>`, freshness metadata, and guard set. Set `VKRELAY2_XWAYLAND_BIN`
only to test or deliberately pin another executable. Distributions outside the validated set need a
compatible system Xwayland or their own security-current baseline.

## Build Outputs

Important release outputs are:

```text
build/linux-release/vkrelay2-launch
build/linux-release/libvulkan_vkrelay2.so
build/linux-release/vkrelay2_icd.json
build/linux-release/vkrelay2-sidecar
build/linux-release/vkrelay2-capture
build/windows-release/vkrelay2-supervisor.exe
build/windows-release/vkrelay2-worker.exe
```

The generated ICD manifest contains the absolute path to its build-tree shared library. Moving only
the manifest or only the shared library breaks loader selection. There is no CMake install target at
present; run from the checkout and let the launcher discover a matching build tree.

## Warnings, Format, and Static Analysis

Project targets use `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic -Werror` on GCC by default.
Disable warning-as-error only for local diagnosis with `-DVKRELAY2_WERROR=OFF`.

The full helper gate checks C/C++ formatting and shell syntax:

```bash
./scripts/dev/rebuild_all.sh --all
```

For a targeted clang-tidy build:

```bash
cmake --preset linux-clang-tidy
cmake --build build/linux-clang-tidy
clang-tidy -p build/linux-clang-tidy common/util/json.cpp
```
