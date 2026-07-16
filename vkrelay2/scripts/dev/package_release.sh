#!/usr/bin/env bash
# vkrelay2: assemble a distributable binary package from the release builds (run from WSL).
#
# The package is a PRUNED MIRROR of the source-tree paths the launcher already resolves
# (linux/launcher/*.sh, build/linux-release, build/windows-release, and the staged private
# Xwayland). That means the shipped launcher is byte-identical to the repo's and needs no
# packaging-specific resolution logic; scripts/packaging/install.sh (copied to the package
# root) does the per-machine steps (runtime apt deps + ICD manifest relocation + checks).
#
# Usage:
#   scripts/dev/package_release.sh            # package existing release builds
#   scripts/dev/package_release.sh --build    # rebuild_all.sh --release first
#
# Output: dist/vkrelay2-<version>-<codename>-<arch>.tar.gz. A RELEASE package fails closed unless it
#         comes from an exact, clean version tag AND a private Xwayland stage the recipe stamped
#         `release_ready: yes`; VKRELAY2_ALLOW_UNRELEASED=1 relaxes both for a dev build. The
#         codename/arch are read from the bundled private Xwayland's PROVENANCE, so the package name and
#         its DISTRO marker always match the Xwayland it carries -- 22.04 jammy, 24.04 noble, 26.04 resolute.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"
fail() { echo "package_release.sh: ERROR: $*" >&2; exit 1; }

if [[ "${1:-}" == "--build" ]]; then
    ./scripts/dev/rebuild_all.sh --release
fi

lin="build/linux-release"
win="build/windows-release"
xwl="build/src_ext/xwayland/stage"
# The project LICENSE/NOTICE live at the git top level (this script runs from the code subdir).
top="$(git -C "${repo_root}" rev-parse --show-toplevel 2>/dev/null || (cd "${repo_root}/.." && pwd))"

# --- verify the inputs exist and are what a user must receive ---------------------------
required=(
    linux/launcher/vkrun
    linux/launcher/lib_private_session.sh
    "${lin}/vkrelay2-launch" "${lin}/vkrelay2-sidecar"
    "${lin}/libvulkan_vkrelay2.so" "${lin}/vkrelay2_icd.json"
    "${win}/vkrelay2-supervisor.exe" "${win}/vkrelay2-worker.exe"
    "${xwl}/Xwayland" "${xwl}/PROVENANCE.txt"
    scripts/packaging/install.sh scripts/packaging/INSTALL.md
    "${top}/LICENSE" "${top}/NOTICE" src_ext/xwayland/LICENSE
)
for f in "${required[@]}"; do
    [[ -e "${f}" ]] || fail "missing ${f} (build both halves with rebuild_all.sh --release,
  and the private Xwayland with src_ext/xwayland/build_private_xwayland.sh)"
done

# Never ship a mock-only worker: that is the exact trap the binary package exists to
# prevent (users would see refused app runs and a refused --list-gpus). The Windows
# configure cache records whether the Vulkan SDK was found.
if grep -q 'Vulkan_LIBRARY:FILEPATH=Vulkan_LIBRARY-NOTFOUND' "${win}/CMakeCache.txt" 2>/dev/null; then
    fail "the Windows build in ${win} was configured WITHOUT the Vulkan SDK (mock-only
  worker). Install the SDK and rebuild before packaging (see docs/building.md)."
fi

# --- release-package admission (fail closed) --------------------------------------------
# A shippable artifact must come from (a) an EXACT, CLEAN version tag, and (b) a distro-matching private
# Xwayland stage the recipe stamped `release_ready: yes`. Either gate alone can produce a package that
# built but is unusable (a dirty/untagged tree, or a stage with an unverified security baseline, skipped
# tests, or an unsupported-baseline override). VKRELAY2_ALLOW_UNRELEASED=1 relaxes BOTH for local/dev
# packaging -- the resulting package is then explicitly a dev build, not a release.
allow_unreleased="${VKRELAY2_ALLOW_UNRELEASED:-0}"
git_dirty=""
git -C "${repo_root}" diff --quiet --ignore-submodules HEAD 2>/dev/null || git_dirty="dirty"
exact_tag="$(git -C "${repo_root}" describe --tags --exact-match 2>/dev/null || true)"
if [[ -n "${exact_tag}" && -z "${git_dirty}" ]]; then
    version="${exact_tag}"
elif [[ "${allow_unreleased}" == "1" ]]; then
    version="$(git -C "${repo_root}" describe --always --dirty --tags 2>/dev/null || echo unversioned)"
    echo "package_release.sh: WARNING: VKRELAY2_ALLOW_UNRELEASED=1 -> building an UNRELEASED dev package" >&2
    echo "package_release.sh: WARNING: version '${version}' is NOT a clean release tag." >&2
else
    fail "refusing to build a release package: HEAD is not an exact, clean version tag${git_dirty:+ (working tree is ${git_dirty})}.
  Tag the release on a clean tree (e.g. 'git tag v0.1.0'), or set VKRELAY2_ALLOW_UNRELEASED=1 for a dev package."
fi

# The bundled Xwayland's PROVENANCE (`target: <codename>/<arch>`) is the authoritative record of
# which distro this package is for -- name the tarball and its DISTRO marker from it, so the two can
# never disagree with the binary actually carried. install.sh checks the extraction host against it.
pkg_target="$(awk '$1 == "target:" { print $2; exit }' "${xwl}/PROVENANCE.txt")"
pkg_codename="${pkg_target%%/*}"
pkg_arch="${pkg_target##*/}"
[[ -n "${pkg_codename}" && -n "${pkg_arch}" && "${pkg_codename}" != "${pkg_arch}" ]] \
    || fail "cannot read 'target: <codename>/<arch>' from ${xwl}/PROVENANCE.txt (rebuild the private Xwayland)"

# The private Xwayland stage must be one the recipe accepted for release (freshness verified, tests run,
# supported baseline). This is the recipe's own verdict, not an inference here.
pkg_release_ready="$(awk '/^release_ready:/{ $1=""; sub(/^[[:space:]]+/,""); print; exit }' "${xwl}/PROVENANCE.txt")"
case "${pkg_release_ready}" in
    yes*) : ;;
    "") [[ "${allow_unreleased}" == "1" ]] || fail "the private Xwayland stage predates the release_ready
  contract (no 'release_ready:' in ${xwl}/PROVENANCE.txt). Rebuild it with the current
  src_ext/xwayland/build_private_xwayland.sh, or set VKRELAY2_ALLOW_UNRELEASED=1 for a dev package." ;;
    *) [[ "${allow_unreleased}" == "1" ]] || fail "the private Xwayland stage is not release-ready:
  release_ready: ${pkg_release_ready}
  Rebuild it cleanly (verified freshness + tests run on a supported baseline), or set
  VKRELAY2_ALLOW_UNRELEASED=1 to package it anyway as a dev build." ;;
esac
name="vkrelay2-${version}-${pkg_codename}-${pkg_arch}"
stage_dir="$(mktemp -d)/${name}"
trap 'rm -rf "$(dirname "${stage_dir}")"' EXIT
mkdir -p "${stage_dir}"

# --- assemble the mirror layout ----------------------------------------------------------
install -D -m 0755 linux/launcher/vkrun \
    "${stage_dir}/linux/launcher/vkrun"
install -D -m 0755 linux/launcher/lib_private_session.sh \
    "${stage_dir}/linux/launcher/lib_private_session.sh"
install -D -m 0755 "${lin}/vkrelay2-launch"  "${stage_dir}/${lin}/vkrelay2-launch"
install -D -m 0755 "${lin}/vkrelay2-sidecar" "${stage_dir}/${lin}/vkrelay2-sidecar"
install -D -m 0755 "${lin}/libvulkan_vkrelay2.so" "${stage_dir}/${lin}/libvulkan_vkrelay2.so"
# Shipped as a placeholder; install.sh regenerates it with the extraction's absolute path.
install -D -m 0644 "${lin}/vkrelay2_icd.json" "${stage_dir}/${lin}/vkrelay2_icd.json"
install -D -m 0755 "${win}/vkrelay2-supervisor.exe" "${stage_dir}/${win}/vkrelay2-supervisor.exe"
install -D -m 0755 "${win}/vkrelay2-worker.exe"     "${stage_dir}/${win}/vkrelay2-worker.exe"
# stage/ may be a symlink into gen/ -- materialize the CONTENT (a package has no generations).
install -D -m 0755 "${xwl}/Xwayland"       "${stage_dir}/${xwl}/Xwayland"
install -D -m 0644 "${xwl}/PROVENANCE.txt" "${stage_dir}/${xwl}/PROVENANCE.txt"
install -D -m 0755 scripts/packaging/install.sh "${stage_dir}/install.sh"
install -D -m 0644 scripts/packaging/INSTALL.md "${stage_dir}/INSTALL.md"
# License material MUST travel with the distribution: Apache-2.0 requires the LICENSE + NOTICE to
# accompany copies, and the bundled Xwayland binary is MIT/X11 whose notice conditions apply in
# binary form -- ship its license under an unambiguous third-party path.
install -D -m 0644 "${top}/LICENSE" "${stage_dir}/LICENSE"
install -D -m 0644 "${top}/NOTICE"  "${stage_dir}/NOTICE"
install -D -m 0644 src_ext/xwayland/LICENSE "${stage_dir}/third_party/xwayland/LICENSE"
printf '%s\n' "${version}" > "${stage_dir}/VERSION"
# The distro this package's bundled Xwayland targets; install.sh fail-closes if the extraction host
# does not match (the private Xwayland is distro-specific).
printf '%s/%s\n' "${pkg_codename}" "${pkg_arch}" > "${stage_dir}/DISTRO"

mkdir -p dist
tar -C "$(dirname "${stage_dir}")" -czf "dist/${name}.tar.gz" "${name}"
echo "package_release.sh: wrote dist/${name}.tar.gz"
tar -tzf "dist/${name}.tar.gz" | sed 's/^/  /'
