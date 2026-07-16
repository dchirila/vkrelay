# Installing From a Binary Package

A vkrelay2 binary package contains prebuilt binaries for Windows x64 + WSL2 amd64: the
Windows supervisor/worker, the Linux launcher/ICD/sidecar, and a provenance-stamped private
Xwayland build for one specific Ubuntu release — **22.04 (jammy)**, **24.04 (noble)**, or
**26.04 (resolute)** — recorded in the package's `DISTRO` marker. Installing one requires
**no compilers and no SDKs** — only runtime packages. Because the private Xwayland is
distro-specific, install the package built for the Ubuntu release you run (the installer
fail-closes on a mismatch).

## Producing a package (maintainers)

From WSL, with both release halves and the private Xwayland built:

```bash
./scripts/dev/package_release.sh            # or --build to rebuild first
```

This writes `dist/vkrelay2-<version>-<codename>-<arch>.tar.gz`, where the codename/arch are
read from the bundled private Xwayland's provenance (so the package name always matches the
Xwayland it carries — build the recipe on the distro you are packaging for). The script
refuses to package a mock-only worker (a Windows build configured without the Vulkan SDK), so
a shipped package always has real GPU enumeration.

The package layout is a pruned mirror of the source-tree paths the launcher resolves
(`linux/launcher/`, `build/linux-release/`, `build/windows-release/`,
`build/src_ext/xwayland/stage/`), so the shipped launcher is byte-identical to the repo's.

## Installing (users)

Prerequisites on the Windows side (one-time):

1. Windows 11 with WSL2 and an Ubuntu 22.04, 24.04, or 26.04 distribution matching the package's
   `DISTRO` marker. All three are validated for session bring-up, the native Vulkan lane, and
   OpenGL through Zink.
2. A Vulkan-capable GPU driver (`vulkaninfo --summary` in PowerShell to verify).
3. The Microsoft Visual C++ x64 redistributable (usually already present).
4. Recommended: mirrored WSL networking (`[wsl2] networkingMode=mirrored` in
   `%UserProfile%\.wslconfig`, then `wsl --shutdown` once).

Then, inside WSL — the package must live under `/mnt/c` so Windows can execute the
bundled daemon:

```bash
tar -xzf vkrelay2-<version>-<codename>-amd64.tar.gz -C /mnt/c/Users/<you>/
cd /mnt/c/Users/<you>/vkrelay2-<version>-<codename>-amd64
sudo ./install.sh
```

`install.sh` installs the runtime apt dependencies (weston, xwayland, Mesa/zink, the
Vulkan loader, xcb libraries), points the Vulkan ICD manifest at the extracted location,
verifies the bundled private Xwayland against the installed `xwayland` package, and
checks Windows interop and the networking mode.

Run applications:

```bash
./linux/launcher/vkrun --list-gpus
./linux/launcher/vkrun -- glxgears
./linux/launcher/vkrun -- openscad
```

See the package's own `INSTALL.md` for notes on moving the directory, uninstalling, and
what happens when Ubuntu ships a newer xwayland security update than the bundled build.
For failures, [Troubleshooting](troubleshooting.md) applies to packaged installs
unchanged — the launcher and its diagnostics are the same as a source checkout's.
