#!/usr/bin/env bash
# Build a distro-specific vkrelay2 .deb from existing release artifacts (run inside WSL).
#
# Usage:
#   scripts/dev/package_release.sh            # package existing release builds
#   scripts/dev/package_release.sh --build    # rebuild vkrelay2 release halves first (not Xwayland)
#
# The Windows binaries are installed as an inert payload under /usr. On first launch vkrun copies
# and verifies them under the invoking Windows user's LocalAppData, where Windows can execute them.
# The package prerm stops vkrelay2 processes and deletes every version-marked Windows payload on
# remove/upgrade. The private Xwayland remains a separate one-time-per-distro build input.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"
fail() { echo "package_release.sh: ERROR: $*" >&2; exit 1; }

case "${1:-}" in
    "") ;;
    --build) ./scripts/dev/rebuild_all.sh --release ;;
    *) fail "usage: $0 [--build]" ;;
esac

lin="build/linux-release"
win="build/windows-release"
xwl="build/src_ext/xwayland/stage"
top="$(git -C "${repo_root}" rev-parse --show-toplevel 2>/dev/null \
    || (cd "${repo_root}/.." && pwd))"

required=(
    linux/launcher/vkrun linux/launcher/lib_private_session.sh
    "${lin}/vkrelay2-launch" "${lin}/vkrelay2-sidecar" "${lin}/libvulkan_vkrelay2.so"
    "${win}/vkrelay2-supervisor.exe" "${win}/vkrelay2-worker.exe" "${win}/CMakeCache.txt"
    "${xwl}/Xwayland" "${xwl}/PROVENANCE.txt"
    scripts/packaging/remove_windows_payload.sh scripts/packaging/windows_cache_root.sh
    "${top}/LICENSE" "${top}/NOTICE" "${top}/docs/install.md" src_ext/xwayland/LICENSE
)
for f in "${required[@]}"; do
    [[ -e "${f}" ]] || fail "missing ${f} (build both release halves and the private Xwayland first)"
done
command -v dpkg-deb >/dev/null 2>&1 || fail "dpkg-deb not found (install dpkg-dev)"

if grep -q 'Vulkan_LIBRARY:FILEPATH=Vulkan_LIBRARY-NOTFOUND' "${win}/CMakeCache.txt"; then
    fail "the Windows release worker was configured without the Vulkan SDK (mock-only)"
fi

# Release admission: exact clean tag for public assets; explicit opt-out for install/remove testing.
allow_unreleased="${VKRELAY2_ALLOW_UNRELEASED:-0}"
git_dirty="$(git -C "${repo_root}" status --porcelain --untracked-files=normal 2>/dev/null || echo dirty)"
exact_tag="$(git -C "${repo_root}" describe --tags --exact-match 2>/dev/null || true)"
if [[ -n "${exact_tag}" && -z "${git_dirty}" ]]; then
    upstream_version="${exact_tag#v}"
    [[ "${upstream_version}" =~ ^[0-9][A-Za-z0-9.+~]*$ ]] \
        || fail "tag '${exact_tag}' cannot be represented safely as a Debian version"
    display_version="${exact_tag}"
elif [[ "${allow_unreleased}" == "1" ]]; then
    git_id="$(git -C "${repo_root}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
    upstream_version="0.0.0+git$(date -u +%Y%m%d).${git_id}"
    display_version="${upstream_version}-dirty"
    echo "package_release.sh: WARNING: building an UNRELEASED development .deb" >&2
else
    if [[ -z "${exact_tag}" ]]; then
        echo "package_release.sh: release gate: HEAD is not an exact version tag" >&2
    fi
    if [[ -n "${git_dirty}" ]]; then
        echo "package_release.sh: release gate: working tree is not clean:" >&2
        while IFS= read -r status_line; do
            echo "  ${status_line}" >&2
        done <<< "${git_dirty}"
    fi
    fail "release packages require an exact clean tag (or set VKRELAY2_ALLOW_UNRELEASED=1 for testing)"
fi

# The Xwayland provenance is authoritative for distro/architecture and release readiness.
pkg_target="$(awk '$1 == "target:" { print $2; exit }' "${xwl}/PROVENANCE.txt")"
pkg_codename="${pkg_target%%/*}"
pkg_arch="${pkg_target##*/}"
case "${pkg_codename}/${pkg_arch}" in
    jammy/amd64) ubuntu_release="22.04" ;;
    noble/amd64) ubuntu_release="24.04" ;;
    resolute/amd64) ubuntu_release="26.04" ;;
    *) fail "unsupported Xwayland target '${pkg_target:-missing}'" ;;
esac
pkg_release_ready="$(awk '/^release_ready:/{ $1=""; sub(/^[[:space:]]+/,""); print; exit }' \
    "${xwl}/PROVENANCE.txt")"
case "${pkg_release_ready}" in
    yes*) ;;
    *) [[ "${allow_unreleased}" == "1" ]] \
        || fail "private Xwayland is not release-ready: ${pkg_release_ready:-missing verdict}" ;;
esac

deb_version="${upstream_version}-1~ubuntu${ubuntu_release}"
package_id="${deb_version}"
asset="vkrelay2-${pkg_codename}-${pkg_arch}.deb"
work="$(mktemp -d)"
root="${work}/root"
deb_tmp="dist/.${asset}.tmp.$$"
sum_tmp="dist/.${asset}.sha256.tmp.$$"
cleanup() {
    rm -rf "${work}" || true
    rm -f "${deb_tmp}" "${sum_tmp}" || true
    return 0
}
trap cleanup EXIT

mkdir -p "${root}/DEBIAN"
install -D -m 0755 linux/launcher/vkrun \
    "${root}/usr/libexec/vkrelay2/launcher/vkrun"
install -D -m 0644 linux/launcher/lib_private_session.sh \
    "${root}/usr/libexec/vkrelay2/launcher/lib_private_session.sh"
install -D -m 0755 "${lin}/vkrelay2-launch" \
    "${root}/usr/libexec/vkrelay2/launcher/vkrelay2-launch"
install -D -m 0755 "${lin}/vkrelay2-sidecar" \
    "${root}/usr/libexec/vkrelay2/launcher/vkrelay2-sidecar"
install -D -m 0755 "${lin}/libvulkan_vkrelay2.so" \
    "${root}/usr/lib/x86_64-linux-gnu/vkrelay2/libvulkan_vkrelay2.so"
install -D -m 0755 "${xwl}/Xwayland" \
    "${root}/usr/libexec/vkrelay2/xwayland/Xwayland"
install -D -m 0644 "${xwl}/PROVENANCE.txt" \
    "${root}/usr/libexec/vkrelay2/xwayland/PROVENANCE.txt"
install -D -m 0755 "${win}/vkrelay2-supervisor.exe" \
    "${root}/usr/libexec/vkrelay2/windows-payload/vkrelay2-supervisor.exe"
install -D -m 0755 "${win}/vkrelay2-worker.exe" \
    "${root}/usr/libexec/vkrelay2/windows-payload/vkrelay2-worker.exe"
install -D -m 0755 scripts/packaging/remove_windows_payload.sh \
    "${root}/usr/libexec/vkrelay2/remove-windows-payload"
install -D -m 0755 scripts/packaging/windows_cache_root.sh \
    "${root}/usr/libexec/vkrelay2/windows-cache-root"

mkdir -p "${root}/usr/bin"
cat > "${root}/usr/bin/vkrun" <<'EOF'
#!/bin/sh
exec /usr/libexec/vkrelay2/launcher/vkrun "$@"
EOF
chmod 0755 "${root}/usr/bin/vkrun"

cat > "${root}/usr/libexec/vkrelay2/launcher/vkrelay2_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/usr/lib/x86_64-linux-gnu/vkrelay2/libvulkan_vkrelay2.so",
        "api_version": "1.1.0"
    }
}
EOF
printf '%s\n' "${package_id}" > "${root}/usr/libexec/vkrelay2/windows-payload/VERSION"
( cd "${root}/usr/libexec/vkrelay2/windows-payload" \
    && sha256sum vkrelay2-supervisor.exe vkrelay2-worker.exe > SHA256SUMS )

install -D -m 0644 "${top}/docs/install.md" "${root}/usr/share/doc/vkrelay2/INSTALL.md"
install -D -m 0644 "${top}/LICENSE" "${root}/usr/share/doc/vkrelay2/LICENSE"
install -D -m 0644 "${top}/NOTICE" "${root}/usr/share/doc/vkrelay2/NOTICE"
install -D -m 0644 src_ext/xwayland/LICENSE \
    "${root}/usr/share/doc/vkrelay2/xwayland.LICENSE"
printf '%s\n' "${display_version}" > "${root}/usr/share/doc/vkrelay2/VERSION"
printf '%s/%s\n' "${pkg_codename}" "${pkg_arch}" > "${root}/usr/share/doc/vkrelay2/DISTRO"
installed_size="$(du -sk "${root}/usr" | awk '{ print $1 }')"

cat > "${root}/DEBIAN/control" <<EOF
Package: vkrelay2
Version: ${deb_version}
Section: graphics
Priority: optional
Architecture: ${pkg_arch}
Installed-Size: ${installed_size}
Maintainer: vkrelay2 maintainers <dchirila@users.noreply.github.com>
Depends: bash, binutils, coreutils, iproute2, util-linux, weston, xwayland, libgl1-mesa-dri, libvulkan1, mesa-utils, libxcb1, libxcb-composite0, libxcb-shape0, libxcb-xtest0, libxcb-xfixes0
Homepage: https://github.com/dchirila/vkrelay
Description: Run WSL graphics applications through a Windows Vulkan driver
 vkrelay2 includes its Linux Vulkan relay, native Win32 supervisor/worker payload,
 and a guarded private Xwayland for Ubuntu ${ubuntu_release} (${pkg_codename}).
EOF

cat > "${root}/DEBIAN/preinst" <<EOF
#!/bin/sh
set -e
codename="\$(. /etc/os-release 2>/dev/null; printf '%s' "\${VERSION_CODENAME:-}")"
arch="\$(dpkg --print-architecture 2>/dev/null || true)"
if [ "\${codename}/\${arch}" != "${pkg_codename}/${pkg_arch}" ]; then
    echo "vkrelay2: this package targets ${pkg_codename}/${pkg_arch}; host is \${codename:-unknown}/\${arch:-unknown}" >&2
    exit 1
fi
EOF
chmod 0755 "${root}/DEBIAN/preinst"

cat > "${root}/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
cache_root=""
attempt=0
while [ -z "\${cache_root}" ] && [ "\${attempt}" -lt 5 ]; do
    cache_root="\$(/usr/libexec/vkrelay2/windows-cache-root 2>/dev/null || true)"
    attempt=\$((attempt + 1))
    [ -n "\${cache_root}" ] || sleep 0.2
done
if [ -n "\${cache_root}" ]; then
    install -d -m 0755 /var/lib/vkrelay2
    printf '%s\n' "\${cache_root}" > /var/lib/vkrelay2/windows-cache-root.tmp
    mv -f /var/lib/vkrelay2/windows-cache-root.tmp /var/lib/vkrelay2/windows-cache-root
else
    echo "vkrelay2: WARNING: Windows LocalAppData could not be recorded during installation." >&2
    echo "vkrelay2: vkrun and package removal will retry discovery when they run." >&2
fi
want="\$(awk '\$1 == "source_package:" { print \$2; exit }' /usr/libexec/vkrelay2/xwayland/PROVENANCE.txt)"
have="\$(dpkg-query -W -f='\${Version}' xwayland 2>/dev/null || true)"
if [ "\${want}" != "\${have}" ]; then
    echo "vkrelay2: WARNING: bundled Xwayland baseline \${want} differs from installed \${have:-none}." >&2
    echo "vkrelay2: application launch will fail closed until a refreshed vkrelay2 package is installed." >&2
fi
echo "vkrelay2: installed. Run: vkrun --list-gpus"
EOF
chmod 0755 "${root}/DEBIAN/postinst"

cat > "${root}/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
case "\${1:-}" in
    remove|upgrade|deconfigure)
        /usr/libexec/vkrelay2/remove-windows-payload '${package_id}'
        ;;
esac
EOF
chmod 0755 "${root}/DEBIAN/prerm"

mkdir -p dist
dpkg-deb --build --root-owner-group "${root}" "${deb_tmp}" >/dev/null
dpkg-deb --info "${deb_tmp}" >/dev/null
dpkg-deb --contents "${deb_tmp}" >/dev/null
mv -f "${deb_tmp}" "dist/${asset}"
( cd dist && sha256sum "${asset}" ) > "${sum_tmp}"
mv -f "${sum_tmp}" "dist/${asset}.sha256"

echo "package_release.sh: wrote dist/${asset}"
echo "package_release.sh: wrote dist/${asset}.sha256"
dpkg-deb --field "dist/${asset}" Package Version Architecture Depends
