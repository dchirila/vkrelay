# vkrelay2 — binary package install

vkrelay2 runs Linux graphics applications in WSL2 while executing their Vulkan work on a
Windows Vulkan driver, presenting each application window as a native Win32 window. This
package contains prebuilt binaries for **Windows x64 + WSL2 amd64**, and a private Xwayland
built for one specific Ubuntu release — **22.04 (jammy)**, **24.04 (noble)**, or **26.04
(resolute)** — recorded in the package's `DISTRO` file. Install the package built for the
Ubuntu release you run; the installer fail-closes on a mismatch. No compilers or SDKs are
needed.

## Prerequisites (one-time, Windows side)

1. Windows 11 with WSL2 and a matching Ubuntu 22.04, 24.04, or 26.04 distribution.
2. A Vulkan-capable GPU driver (any current NVIDIA/AMD/Intel driver; verify with
   `vulkaninfo --summary` in PowerShell if unsure).
3. The Microsoft Visual C++ x64 redistributable (usually already present; if the daemon
   fails to start with a missing-DLL error, install `vc_redist.x64.exe` from Microsoft).
4. Recommended: mirrored WSL networking — in `%UserProfile%\.wslconfig`:

   ```ini
   [wsl2]
   networkingMode=mirrored
   ```

   then `wsl --shutdown` once. (NAT mode works too, but every run needs
   `VKRELAY2_BIND=0.0.0.0` and a one-time firewall prompt.)

## Install (WSL side)

Extract the package **somewhere under `/mnt/c`** (Windows must be able to reach the
bundled `.exe` files), then run the installer:

```bash
# <codename> is the release this package was built for: jammy, noble, or resolute.
tar -xzf vkrelay2-<version>-<codename>-amd64.tar.gz -C /mnt/c/Users/<you>/
cd /mnt/c/Users/<you>/vkrelay2-<version>-<codename>-amd64
sudo ./install.sh
```

The installer only installs runtime apt packages (weston, xwayland, Mesa, the Vulkan
loader, xcb libraries), points the Vulkan ICD manifest at the extracted location, and
checks the two host capabilities that fail confusingly when broken (Windows interop,
networking mode).

## Run

```bash
./linux/launcher/vkrun --list-gpus       # adapters on the Windows side
./linux/launcher/vkrun -- glxgears       # OpenGL smoke test
./linux/launcher/vkrun -- openscad       # your applications
```

The first run auto-starts the Windows-side daemon; later runs reuse it. If startup fails,
the launcher preserves a diagnostic bundle under `/tmp/vkrelay2-logs-<pid>` and prints the
path.

## Notes

- The bundled private Xwayland is built from this release's exact security-patched
  xwayland source (the `DISTRO` file records which one) and is verified against the
  *installed* `xwayland` package version at launch (fail-closed). Install the package that
  matches your Ubuntu release; the installer refuses a mismatch. If Ubuntu ships a newer
  xwayland security update than this package, app runs will say so — update the package or
  rebuild from a source checkout.
- Moving the extracted directory is fine: re-run `./install.sh` afterwards (it re-points
  the ICD manifest).
- Uninstall: delete the extracted directory and, on the Windows side, stop the daemon
  (`Stop-Process -Name vkrelay2-supervisor,vkrelay2-worker -Force`).
