#!/usr/bin/env bash
# vkrelay2 binary-package installer (run INSIDE WSL, from the extracted package root).
#
# The package is a pruned mirror of the source-tree paths the launcher already resolves
# (linux/launcher/*.sh + build/linux-release + build/windows-release + the staged private
# Xwayland), so no binary needs rebuilding and no launcher code differs from a source
# checkout. This script only:
#   1. sanity-checks the host (WSL2, Ubuntu 22.04 jammy, extraction under /mnt/*);
#   2. installs the RUNTIME apt dependencies (no compilers, no SDKs);
#   3. rewrites the Vulkan ICD manifest's library_path to this extraction's absolute path;
#   4. verifies the staged private Xwayland matches the installed xwayland package;
#   5. checks Windows interop + networking mode (the two host capabilities that fail
#      confusingly when broken) and prints the quickstart commands.
#
# Usage:  sudo ./install.sh          (sudo needed for apt; everything else is user-local)
set -euo pipefail

pkg_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail() { echo "install.sh: ERROR: $*" >&2; exit 1; }
warn() { echo "install.sh: WARNING: $*" >&2; }

# --- 1. host sanity ---------------------------------------------------------------------
grep -qi 'microsoft' /proc/version 2>/dev/null \
    || warn "this does not look like WSL; vkrelay2 supports Windows 11 + WSL2 only."
# The bundled private Xwayland is distro-specific (built from that distro's Xwayland source). The
# package records which distro it is FOR in DISTRO (<codename>/<arch>); refuse to install a package
# whose Xwayland does not match the extraction host -- an app run would otherwise refuse the
# mismatched server later. Supported: 22.04 jammy, 24.04 noble, 26.04 resolute.
codename="$(. /etc/os-release 2>/dev/null; printf '%s' "${VERSION_CODENAME:-}")"
host_arch="$(dpkg --print-architecture 2>/dev/null || echo unknown)"
pkg_distro="$(cat "${pkg_root}/DISTRO" 2>/dev/null || true)"
if [ -n "${pkg_distro}" ]; then
    [ "${pkg_distro}" = "${codename}/${host_arch}" ] \
        || fail "this package's bundled Xwayland is for '${pkg_distro}', but this host is
  '${codename:-unknown}/${host_arch}'. The private Xwayland is distro-specific -- install the package
  built for THIS distro, or rebuild it here with
  src_ext/xwayland/build_private_xwayland.sh (see INSTALL.md)."
else
    # Older package without a DISTRO marker: fall back to the historical jammy-only assumption.
    [ "${codename}" = "jammy" ] \
        || warn "distro codename is '${codename:-unknown}', not jammy, and this package carries no" \
                "DISTRO marker: the bundled private Xwayland may be refused on this distro."
fi
case "${pkg_root}" in
    /mnt/*) : ;;
    *) fail "the package is extracted at '${pkg_root}', which Windows cannot reach. The daemon
  auto-start runs the bundled .exe files through Windows, so the package must live on a
  Windows drive. Re-extract somewhere under /mnt/c (e.g. /mnt/c/Users/<you>/vkrelay2) and
  re-run ./install.sh there." ;;
esac
for f in \
    linux/launcher/vkrun linux/launcher/lib_private_session.sh \
    build/linux-release/vkrelay2-launch build/linux-release/vkrelay2-sidecar \
    build/linux-release/libvulkan_vkrelay2.so build/linux-release/vkrelay2_icd.json \
    build/windows-release/vkrelay2-supervisor.exe build/windows-release/vkrelay2-worker.exe \
    build/src_ext/xwayland/stage/Xwayland build/src_ext/xwayland/stage/PROVENANCE.txt \
    LICENSE NOTICE third_party/xwayland/LICENSE; do
    [ -e "${pkg_root}/${f}" ] || fail "package is incomplete: missing ${f}"
done

# --- 2. runtime apt dependencies (no build tools) ---------------------------------------
# weston + xwayland: the private per-app display.  libgl1-mesa-dri: Mesa incl. the zink
# driver (GL -> Vulkan).  libvulkan1: the Vulkan loader the app-side ICD plugs into.
# libxcb*: sidecar runtime (WM/chrome/input).  mesa-utils: glxgears for the smoke test.
runtime_deps=(weston xwayland libgl1-mesa-dri libvulkan1 mesa-utils
              libxcb1 libxcb-composite0 libxcb-shape0 libxcb-xtest0 libxcb-xfixes0)
if [ "$(id -u)" -eq 0 ]; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${runtime_deps[@]}"
else
    echo "install.sh: installing runtime packages with sudo (${runtime_deps[*]})"
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${runtime_deps[@]}"
fi

# --- 3. relocate the ICD manifest -------------------------------------------------------
# The manifest carries an absolute library_path (the loader reads it via VK_ICD_FILENAMES);
# point it at THIS extraction. Regenerated wholesale so re-running install.sh after moving
# the directory always heals it.
icd_so="${pkg_root}/build/linux-release/libvulkan_vkrelay2.so"
cat > "${pkg_root}/build/linux-release/vkrelay2_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "${icd_so}",
        "api_version": "1.1.0"
    }
}
EOF

# --- 4. staged Xwayland vs installed package --------------------------------------------
# The launcher fail-closed-verifies the staged private Xwayland against the INSTALLED
# xwayland package (same jammy security version). Surface a mismatch now, not at first run.
prov="${pkg_root}/build/src_ext/xwayland/stage/PROVENANCE.txt"
want="$(awk '$1 == "source_package:" { print $2; exit }' "${prov}")"
have="$(dpkg-query -W -f='${Version}' xwayland 2>/dev/null || true)"
if [ "${want}" != "${have}" ]; then
    warn "the bundled private Xwayland was built from xwayland ${want} but this host has
  '${have:-none}' installed. The launcher will refuse the bundled build (fail-closed) and,
  if the installed stock build is the known-crashing one, refuse that too. If app runs
  refuse to start, rebuild the private Xwayland from a source checkout
  (src_ext/xwayland/build_private_xwayland.sh) or wait for a package refresh."
fi

# --- 5. host capability checks + quickstart ---------------------------------------------
if ! powershell.exe -NoProfile -NonInteractive -Command 'exit 0' </dev/null >/dev/null 2>&1; then
    warn "cannot execute Windows binaries from WSL (WSLInterop binfmt registration missing).
  The daemon cannot auto-start until this is fixed. Re-register as root:
    echo ':WSLInterop:M::MZ::/init:PF' > /proc/sys/fs/binfmt_misc/register
  and make it durable in /etc/wsl.conf:
    [boot]
    command = /bin/sh -c \"[ -e /proc/sys/fs/binfmt_misc/WSLInterop ] || echo :WSLInterop:M::MZ::/init:PF > /proc/sys/fs/binfmt_misc/register\""
fi
if command -v wslinfo >/dev/null 2>&1 \
    && [ "$(wslinfo --networking-mode 2>/dev/null)" = "nat" ]; then
    warn "WSL networking mode is NAT. Recommended: mirrored networking
  ([wsl2] networkingMode=mirrored in %UserProfile%\\.wslconfig, then wsl --shutdown).
  Otherwise every run needs VKRELAY2_BIND=0.0.0.0 (one Windows Firewall prompt)."
fi

echo
echo "install.sh: done. Try it:"
echo "  ${pkg_root}/linux/launcher/vkrun --list-gpus"
echo "  ${pkg_root}/linux/launcher/vkrun -- glxgears"
echo "  ${pkg_root}/linux/launcher/vkrun -- <your app>"
