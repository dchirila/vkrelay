# Installing a Prebuilt Package

A vkrelay2 package contains everything built by this project for Windows x64 and WSL2 amd64:
the Windows supervisor/worker, Linux launcher/ICD/sidecar, and a guarded private Xwayland. Users
need runtime packages and a Vulkan-capable Windows GPU driver, but do **not** need compilers,
Visual Studio, the Vulkan SDK, or an Xwayland source build.

The private Xwayland is tied to the Ubuntu release and current `xwayland` security package. Choose
the asset matching the Ubuntu distribution in which vkrelay2 will run:

| WSL Ubuntu release | Codename | GitHub Release asset |
|---|---|---|
| 22.04 | `jammy` | `vkrelay2-jammy-amd64.tar.gz` |
| 24.04 | `noble` | `vkrelay2-noble-amd64.tar.gz` |
| 26.04 | `resolute` | `vkrelay2-resolute-amd64.tar.gz` |

The installer checks the package's `DISTRO` marker and refuses the wrong asset.

## Download and install

Windows-side prerequisites:

1. Windows 11 with WSL2 and one of the supported Ubuntu releases above.
2. A Vulkan-capable NVIDIA, AMD, or Intel Windows GPU driver.
3. The Microsoft Visual C++ x64 redistributable, which is usually already installed.
4. Recommended: mirrored WSL networking. Put `networkingMode=mirrored` under `[wsl2]` in
   `%UserProfile%\.wslconfig`, then run `wsl --shutdown` once from Windows.

In WSL, select the matching asset and download it to a directory on a Windows drive. Replace
`<Windows-user>` below with your Windows account directory name:

```bash
CODENAME="$(. /etc/os-release && printf '%s' "${VERSION_CODENAME}")"
case "${CODENAME}" in
    jammy|noble|resolute) ;;
    *) echo "Unsupported Ubuntu release: ${CODENAME}" >&2; exit 1 ;;
esac
ARCH="$(dpkg --print-architecture)"
[ "${ARCH}" = "amd64" ] || { echo "Unsupported architecture: ${ARCH}" >&2; exit 1; }

ASSET="vkrelay2-${CODENAME}-${ARCH}.tar.gz"
BASE="https://github.com/dchirila/vkrelay/releases/latest/download"
DEST="/mnt/c/Users/<Windows-user>/Downloads"

mkdir -p "${DEST}"
cd "${DEST}"
curl -fLO "${BASE}/${ASSET}"
curl -fLO "${BASE}/${ASSET}.sha256"
sha256sum -c "${ASSET}.sha256"

PACKAGE_DIR="$(tar -tzf "${ASSET}" | head -n 1 | cut -d/ -f1)"
tar -xzf "${ASSET}"
cd "${PACKAGE_DIR}"
./install.sh
```

If preferred, download the archive and its `.sha256` file with a browser from the
[latest GitHub Release](https://github.com/dchirila/vkrelay/releases/latest), then start with the
`sha256sum` command above. The package must remain somewhere under `/mnt/c` (or another mounted
Windows drive) because Windows must execute its bundled `.exe` files.

If `curl` is not installed, install it with `sudo apt update && sudo apt install curl`, or use the
browser path.

`install.sh` uses `sudo` only to install runtime APT dependencies (Weston, Xwayland, Mesa/Zink, the
Vulkan loader, and XCB libraries). It also relocates the Vulkan ICD manifest to the extraction path,
checks Windows interop/networking, and verifies the bundled Xwayland against the installed Ubuntu
package.

## Run

From the extracted package directory:

```bash
./linux/launcher/vkrun --list-gpus
./linux/launcher/vkrun -- glxgears
./linux/launcher/vkrun -- openscad
```

The first command is the best end-to-end GPU-driver check because it queries the actual bundled
Windows worker. See the package's `INSTALL.md` for moving or uninstalling it. For failures,
[Troubleshooting](troubleshooting.md) applies unchanged.

## Build the release assets (maintainers)

Packages must come from an exact, clean version tag. Do not move an existing tag to newer code;
create the next patch/minor tag after the full release gate passes. The Windows release binaries
can be built once and reused, but the Linux release and private Xwayland must be built separately
inside each supported WSL Ubuntu distribution.

On the tagged shared checkout, first build the Windows half once from any WSL distribution:

```bash
./scripts/dev/rebuild_all.sh --release --windows-only --clean
```

Then run the following in **each** of jammy, noble, and resolute. Because the checkout is under
`/mnt/c`, all three distributions write their finished archives into the same `dist/` directory:

```bash
sudo apt update
VKRELAY2_XWL_INSTALL_DEPS=1 \
    bash src_ext/xwayland/build_private_xwayland.sh
./scripts/dev/rebuild_all.sh --release --linux-only --clean
./scripts/dev/package_release.sh
```

The packager fail-closes unless the Windows worker has real Vulkan support, the source is an exact
clean tag, and the private-Xwayland provenance says `release_ready: yes`. Each run writes a stable
GitHub asset name plus its checksum while retaining the version in the archive's root directory and
`VERSION` marker:

```text
dist/vkrelay2-jammy-amd64.tar.gz
dist/vkrelay2-jammy-amd64.tar.gz.sha256
dist/vkrelay2-noble-amd64.tar.gz
dist/vkrelay2-noble-amd64.tar.gz.sha256
dist/vkrelay2-resolute-amd64.tar.gz
dist/vkrelay2-resolute-amd64.tar.gz.sha256
```

Verify all assets, then create a draft GitHub Release with the authenticated GitHub CLI:

```bash
(cd dist && sha256sum -c vkrelay2-*-amd64.tar.gz.sha256)

TAG="$(git describe --tags --exact-match)"
git push origin "${TAG}"
gh release create "${TAG}" \
    dist/vkrelay2-{jammy,noble,resolute}-amd64.tar.gz{,.sha256} \
    --draft --verify-tag --generate-notes --title "vkrelay2 ${TAG}"
```

Use `gh release download "${TAG}"` to fetch draft assets into a clean WSL test location, then install
and smoke-test at least one of them. When ready, publish the draft and mark it as latest:

```bash
gh release edit "${TAG}" --draft=false --latest
```

If Ubuntu publishes a newer `xwayland` security revision, the affected bundled binary will
intentionally stop matching fully updated hosts. Rebuild that distro's package and publish a new
vkrelay2 patch release rather than replacing an asset in an already published release.
