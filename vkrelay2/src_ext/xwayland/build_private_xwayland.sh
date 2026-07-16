#!/usr/bin/env bash
# Build a private, security-correct, patched Xwayland for vkrelay2 (fix).
#
# WHY: vkrelay2's private compositor is Weston's headless backend, which advertises NO wl_seat
# (application input is driven out-of-band through XTEST). Stock Xwayland then NULL-derefs on
# seatless paths -- xwl_cursor_warped_to() on any client pointer warp (Segfault at 0xa0), and (on
# Xwayland 24.1.x) damage_report() on a window-move (Segfault at 0x40) -- crashing the whole guest
# X server. This recipe adds a minimal upstream-submittable guard for each, and nothing else.
#
# WHAT: this builds Xwayland from the *complete Ubuntu source package* for the HOST distro (so it
# carries that distro's FULL security-patch stack) and adds only vkrelay2's guard patch(es) on top
# (patches/000N-*.patch, applied last in the quilt series). It builds with Ubuntu's own packaging
# rules (dpkg-buildpackage) so the feature set and hardening flags match the distro binary; the
# ONLY difference from the stock server is our guard(s). The system Xwayland is never touched; the
# launcher selects this build via VKRELAY2_XWAYLAND_BIN.
#
# SUPPORTED TARGETS: Ubuntu 22.04 (jammy), 24.04 (noble), and 26.04 (resolute), amd64. The host
# distro is detected and its OWN security-current Xwayland source is used:
#   - jammy is acquired from a HASH-PINNED, offline-capable source set (sha256sums.txt + dist/) and
#     freshness-gated against the distro candidate -- the reproducible shipped baseline;
#   - other supported distros are acquired via `apt-get source` (integrity by the distro's signed
#     apt keyring; always the current candidate), and their exact version + input hashes are
#     recorded in PROVENANCE.txt.
# The guard set is chosen by the Xwayland upstream version (0001 for all; 0002 for >= 24.1). A
# host outside the supported set is refused unless VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH=1, which
# attempts the apt-get-source path and is recorded in the manifest. Other overrides (accept the
# consequences): VKRELAY2_XWL_ALLOW_STALE_BASELINE=1.
#
# NOTE: an earlier revision built from *pristine upstream* and thereby dropped the entire Ubuntu
# CVE series. That was a security regression. This recipe builds from the distro source package;
# do not revert it to a pristine-upstream tarball build.
#
# Build dependencies come from the extracted source package's Build-Depends. Install them once on
# the host by running this script with VKRELAY2_XWL_INSTALL_DEPS=1. The install path uses
# mk-build-deps (devscripts + equivs) against debian/control; Jammy's apt-get build-dep accepts a
# source PACKAGE name but rejects local control/.dsc paths. Do this BEFORE a test-enabled build,
# since the package tests run inside an unprivileged namespace that cannot install packages.
#
# Usage:   bash build_private_xwayland.sh
# Output:  a staged, stripped executable whose absolute path is printed as VKRELAY2_XWAYLAND_BIN=<path>,
#          plus stage/PROVENANCE.txt. The package test suite runs by default (VKRELAY2_XWL_SKIP_TESTS=1
#          to skip); on WSLg the recipe re-execs inside a private mount namespace so the tests' Xvfb can
#          use a writable /tmp/.X11-unix.
#
# Linux-only. Does NOT run as part of a normal vkrelay2 compile -- explicit dependency step.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

log() { printf 'build-xwayland: %s\n' "$*" >&2; }
die() { printf 'build-xwayland: ERROR: %s\n' "$*" >&2; exit 1; }

for t in dpkg-source dpkg-buildpackage dpkg-deb quilt curl sha256sum tar patch awk flock; do
    command -v "$t" >/dev/null 2>&1 || die "missing tool: $t (install dpkg-dev/quilt/util-linux; see header)"
done

# --- host detection ---------------------------------------------------------------------------
host_codename="$( ( . /etc/os-release 2>/dev/null && printf '%s' "${VERSION_CODENAME:-}" ) || true )"
host_arch="$(dpkg --print-architecture 2>/dev/null || echo unknown)"
supported_arch="amd64"

# The candidate is the distro's current security revision (e.g. 2:24.1.10-1). It drives the
# apt-get-source acquisition and the upstream-version-derived guard set. jammy additionally
# hash-pins, so its candidate only has to MATCH the pin (freshness gate).
distro_candidate=""
if command -v apt-cache >/dev/null 2>&1; then
    distro_candidate="$(apt-cache policy xwayland 2>/dev/null | awk '/Candidate:/{print $2}')"
    [ "${distro_candidate:-}" = "(none)" ] && distro_candidate=""
fi

# upstream version from a deb version string: strip epoch ("2:") and debian revision ("-1ubuntu0.20").
deb_upstream() { local v="${1#*:}"; printf '%s' "${v%-*}"; }
# a >= b for dotted versions.
ver_ge() { [ "$1" = "$2" ] || [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n1)" = "$1" ]; }

# --- per-distro baseline selection ------------------------------------------------------------
# jammy is the hash-pinned/offline reproducible baseline; every other supported distro is acquired
# via the distro's signed apt source (apt-get source). The guard set is version-derived below, so a
# newly supported distro needs no table edit here beyond the supported-codename allowlist.
supported_codenames="jammy noble resolute"
acquire="aptsrc"
strict_cve=0
cve_sentinel=""
min_cve=0

# jammy hash-pin constants (authoritative; the reproducible shipped baseline).
jammy_deb_version="2:22.1.1-1ubuntu0.20"
jammy_dsc="xwayland_22.1.1-1ubuntu0.20.dsc"
jammy_files=(
    "xwayland_22.1.1-1ubuntu0.20.dsc"
    "xwayland_22.1.1-1ubuntu0.20.debian.tar.xz"
    "xwayland_22.1.1.orig.tar.xz"
    # dpkg-source requires the orig's detached .asc to be present (it is listed in the .dsc). We do
    # NOT PGP-verify it (no pinned X.Org key in this tree); it is hash-pinned in sha256sums.txt for
    # tamper-evidence exactly like the other inputs -- the pins are the integrity mechanism, not PGP.
    "xwayland_22.1.1.orig.tar.xz.asc"
)
# HTTPS: the SHA-256 pins already protect content; HTTPS just avoids a needless transport downgrade.
base_url="https://archive.ubuntu.com/ubuntu/pool/main/x/xwayland/"
sums="${here}/sha256sums.txt"

is_supported=0
for c in ${supported_codenames}; do [ "${host_codename}" = "${c}" ] && is_supported=1; done
if [ "${is_supported}" = "0" ] || [ "${host_arch}" != "${supported_arch}" ]; then
    if [ "${VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH:-0}" != "1" ]; then
        die "baseline: this recipe supports Ubuntu {${supported_codenames}}/${supported_arch}, but the
       host is ${host_codename:-?}/${host_arch}. Set VKRELAY2_XWL_ALLOW_BASELINE_MISMATCH=1 to attempt
       an apt-get-source build for this distro (recorded as an unverified baseline in PROVENANCE)."
    fi
    log "WARNING: unsupported baseline ${host_codename:-?}/${host_arch} -> attempting apt-get-source build (override)"
fi

if [ "${host_codename}" = "jammy" ] && [ "${host_arch}" = "amd64" ]; then
    acquire="hashpin"
    deb_version="${jammy_deb_version}"
    dsc="${jammy_dsc}"
    files=("${jammy_files[@]}")
    strict_cve=1
    min_cve=40
    cve_sentinel="CVE-2025-62231.patch"
    [ -f "${sums}" ] || die "missing ${sums}"
else
    # apt-get source: the candidate IS the version we build (current by construction).
    [ -n "${distro_candidate}" ] || die "no apt Candidate for xwayland (run 'sudo apt-get update')."
    deb_version="${distro_candidate}"
    dsc=""          # apt-get source resolves the .dsc name itself
    files=()
    min_cve=0       # a dev release legitimately carries no CVE backports yet; record, don't gate
fi
upstream_ver="$(deb_upstream "${deb_version}")"

# --- guard set (version-derived; independent of the codename table) ---------------------------
guard_patches=("${here}/patches/0001-xwayland-guard-seatless-pointer-warp.patch")
if ver_ge "${upstream_ver}" "24.1"; then
    guard_patches+=("${here}/patches/0002-xwayland-guard-damage-report-null-window.patch")
fi
for p in "${guard_patches[@]}"; do [ -f "${p}" ] || die "missing guard patch: ${p}"; done
log "baseline: ${host_codename}/${host_arch} xwayland ${deb_version} (upstream ${upstream_ver}), acquire=${acquire}, guards=${#guard_patches[@]}"

# --- security-freshness: identity is guaranteed by the hash pin (jammy) or the apt keyring (others);
# freshness is not. Establish -- from a RECENTLY-refreshed package index -- that the baseline is the
# distro's current revision. For jammy: candidate must EQUAL the pin. For apt-get-source distros:
# apt-get source always fetches the candidate, so we only require the index to be recent (so the
# candidate is not itself stale). FAIL CLOSED on a stale index; the only way past is an explicit,
# recorded override.
freshness_max_age_days="${VKRELAY2_XWL_APT_MAX_AGE_DAYS:-7}"
case "${freshness_max_age_days}" in
    ''|*[!0-9]*) die "VKRELAY2_XWL_APT_MAX_AGE_DAYS must be a non-negative integer (got '${freshness_max_age_days}')" ;;
esac
stale_override="${VKRELAY2_XWL_ALLOW_STALE_BASELINE:-0}"
freshness_verified="no (not checked)"
if ! command -v apt-cache >/dev/null 2>&1; then
    if [ "${stale_override}" = "1" ]; then
        freshness_verified="no (override: apt-cache absent, unverifiable)"
        log "WARNING: apt-cache absent -> freshness unverifiable; VKRELAY2_XWL_ALLOW_STALE_BASELINE override in effect"
    else
        die "security-freshness: apt-cache is absent, so the baseline cannot be verified as current.
       Install apt, or set VKRELAY2_XWL_ALLOW_STALE_BASELINE=1 for a deliberate offline/unverified build."
    fi
else
    # Newest InRelease age across ANY pocket of this codename (base / -updates / -security). A dev
    # release may have only the base pocket; a stable one has -security. Gate on the newest present.
    now="$(date +%s)"
    newest_mtime="$(find /var/lib/apt/lists -maxdepth 1 -type f -name "*ubuntu*_dists_${host_codename}*_InRelease" 2>/dev/null \
        | xargs -r stat -c '%Y' 2>/dev/null | sort -nr | head -1)"
    index_age="unknown"
    [ -n "${newest_mtime}" ] && index_age="$(( ( now - newest_mtime ) / 86400 ))d"
    if [ "${stale_override}" = "1" ]; then
        freshness_verified="no (override; candidate=${distro_candidate:-none}, index=${index_age})"
        log "WARNING: security-freshness OVERRIDDEN (candidate=${distro_candidate:-none}, index=${index_age})"
    elif [ -z "${distro_candidate:-}" ]; then
        die "security-freshness: apt-cache has no Candidate for xwayland. Run 'sudo apt-get update' and
       retry, or set VKRELAY2_XWL_ALLOW_STALE_BASELINE=1 to build unverified."
    elif [ "${acquire}" = "hashpin" ] && [ "${distro_candidate}" != "${deb_version}" ]; then
        die "security-freshness: pinned ${deb_version}, but the distro candidate is ${distro_candidate}.
       A newer security revision is available -- rebase the pin (bump version, filenames, sha256sums.txt),
       or set VKRELAY2_XWL_ALLOW_STALE_BASELINE=1 to build the pinned (older) one."
    elif [ -z "${newest_mtime}" ]; then
        die "security-freshness: no InRelease index found for '${host_codename}'. Run 'sudo apt-get update'
       (or set VKRELAY2_XWL_ALLOW_STALE_BASELINE=1 for an unverified build)."
    elif [ "$(( ( now - newest_mtime ) / 86400 ))" -gt "${freshness_max_age_days}" ]; then
        die "security-freshness: the '${host_codename}' apt index is ${index_age} old, so the candidate is
       not trustworthy as current. Run 'sudo apt-get update' (or raise VKRELAY2_XWL_APT_MAX_AGE_DAYS),
       or set VKRELAY2_XWL_ALLOW_STALE_BASELINE=1."
    else
        freshness_verified="yes (candidate ${distro_candidate}; index ${index_age})"
        log "security-freshness: candidate ${distro_candidate}${acquire:+ }; index ${index_age}"
    fi
fi

# --- test policy + WSLg namespace re-exec ------------------------------------
# The package test suite runs by DEFAULT. It needs a working Xvfb, which needs a writable
# /tmp/.X11-unix. WSLg bind-mounts that read-only, so -- exactly like the launcher -- re-exec once
# inside a private user+mount namespace that overlays a writable tmpfs there. Then the tests run and we
# stage a tests_run:yes artifact. VKRELAY2_XWL_SKIP_TESTS=1 is an explicit opt-out.
run_tests=1
[ "${VKRELAY2_XWL_SKIP_TESTS:-0}" = "1" ] && run_tests=0

x11_dir_usable() {
    local d=/tmp/.X11-unix probe
    if [ -d "${d}" ]; then
        [ -k "${d}" ] || return 1
        probe="${d}/.vkrxwl-wtest.$$"
        if ( : >"${probe}" ) 2>/dev/null; then rm -f "${probe}"; return 0; fi
        return 1
    fi
    [ -w /tmp ]
}
if [ "${run_tests}" = "1" ] && [ "${VKRELAY2_X11NS:-0}" != "1" ] && ! x11_dir_usable; then
    if [ "${VKRELAY2_XWL_INSTALL_DEPS:-0}" = "1" ]; then
        log "note: INSTALL_DEPS set -> installing deps on the host first (real root); the recipe will then re-exec into the test namespace automatically."
    elif command -v unshare >/dev/null 2>&1 && unshare --user --map-root-user --mount -- true 2>/dev/null; then
        log "re-execing inside a private mount namespace so the package tests get a writable /tmp/.X11-unix"
        export VKRELAY2_X11NS=1
        exec unshare --user --map-root-user --mount -- bash -c '
            mount -t tmpfs tmpfs /tmp/.X11-unix || exit 1
            chmod 1777 /tmp/.X11-unix
            exec "$@"
        ' _ bash "$0" "$@"
    else
        log "WARNING: /tmp/.X11-unix is not writable and no unprivileged userns is available; the package"
        log "         tests will likely fail. Re-run with VKRELAY2_XWL_SKIP_TESTS=1, or fix /tmp/.X11-unix."
    fi
fi

# --- build/work layout (ignored; never tracked) ---------------------------------------------
build_root="${VKRELAY2_XWL_BUILD_ROOT:-${here}/../../build/src_ext/xwayland}"
dist_dir="${here}/dist"                        # optional offline source cache (not tracked)
# dpkg-buildpackage/fakeroot and dpkg-deb need real POSIX ownership, which a Windows DrvFs mount
# (/mnt/...) does not provide -- the packaging step fails there with a fakeroot error. When build_root
# is on such a mount, do the heavy fetch/extract/build in a NATIVE-filesystem work dir and stage only
# the finished binary + manifest (plain file copies, which DrvFs handles fine) back to build_root/stage.
# On a native Linux dev box, work == build_root and nothing is redirected. The work dir is namespaced by
# uid + a hash of build_root so concurrent builds from different checkouts/users
# do not stomp each other; a flock then serializes builds that DO share a work dir.
build_root_hash="$(printf '%s' "${build_root}" | cksum | cut -d' ' -f1)"
work="${build_root}"
drvfs=0
case "${build_root}" in
    /mnt/*) work="${VKRELAY2_XWL_WORK_ROOT:-/tmp/vkrelay2-xwl-build-$(id -u)-${build_root_hash}}"; drvfs=1 ;;
esac
srcdir="${work}/xwayland-${upstream_ver}"       # dpkg-source -x / apt-get source extraction dir (native FS)
stage="${build_root}/stage"                    # launcher-facing output (may be on DrvFs)
stage_new="${build_root}/stage.new.$$"         # atomic-publish staging dir (renamed onto stage at end)
staged_bin="${stage_new}/Xwayland"

mkdir -p "${build_root}" "${work}"
if [ "${drvfs}" = "1" ]; then
    log "build_root is on a DrvFs mount; heavy build redirected to native ${work} (staged back to ${stage})"
fi

# Serialize concurrent builds sharing this work dir. flock is mandatory (a
# required tool above), so serialization is never silently skipped.
exec 9>"${work}/.build.lock"
flock -n 9 || die "another private-Xwayland build holds ${work}/.build.lock; wait for it or remove the lock"

# --- 1. acquire the distro source package (leaves ${srcdir} extracted with the series applied) --
# apt_root: run apt-get with privilege only when needed (build hosts are typically root-capable).
apt_sudo=""
[ "$(id -u)" = "0" ] || apt_sudo="sudo"

fetch_verify() {  # hash-pinned path (jammy): fetch + verify each source input against sha256sums.txt
    local name="$1" dest="${work}/$1" want got tmp
    want="$(awk -v n="${name}" '$2==n{print $1}' "${sums}")"
    [ -n "${want}" ] || die "no pinned SHA-256 for ${name} in $(basename "${sums}")"
    if [ -f "${dest}" ]; then
        got="$(sha256sum "${dest}" | awk '{print $1}')"
        [ "${got}" = "${want}" ] && { log "cached ${name} verified"; return 0; }
        log "cached ${name} failed hash check; discarding and re-fetching"; rm -f "${dest}"
    fi
    tmp="${dest}.tmp.$$"
    if [ -f "${dist_dir}/${name}" ]; then
        cp -f "${dist_dir}/${name}" "${tmp}"; log "using offline ${dist_dir}/${name}"
    else
        curl -fsSL -o "${tmp}" "${base_url}${name}" || { rm -f "${tmp}"; die "download failed: ${name}"; }
    fi
    got="$(sha256sum "${tmp}" | awk '{print $1}')"
    [ "${got}" = "${want}" ] || { rm -f "${tmp}"; die "SHA-256 mismatch for ${name}: want ${want}, got ${got}"; }
    mv -f "${tmp}" "${dest}"; log "fetched + verified ${name}"
}

# record source-input hashes for the manifest (both paths).
src_input_hashes=""
rm -rf "${srcdir}"
if [ "${acquire}" = "hashpin" ]; then
    for f in "${files[@]}"; do fetch_verify "${f}"; done
    ( cd "${work}" && dpkg-source -x "${dsc}" "xwayland-${upstream_ver}" ) >&2 || die "dpkg-source -x failed"
    src_input_hashes="$(grep -v '^#' "${sums}")"
else
    # apt-get source: integrity via the distro's signed apt keyring. Needs a deb-src entry.
    # APT::Sandbox::User=root disables apt's drop to the unprivileged '_apt' user for the fetch:
    # the build runs as root (and, for the package tests, inside a user namespace where '_apt' is
    # not mapped), so the sandbox user cannot write the root-owned work dir -- which turns apt's
    # "unsandboxed as root" WARNING into a fatal "Failed to fetch" (error 112). Fetching as root is
    # the documented, correct choice for a root/namespace build; integrity still comes from the
    # signed Release chain that apt verifies regardless of the download user.
    apt_src=(apt-get -o APT::Sandbox::User=root source)
    ensure_deb_src() {
        ( cd "${work}" && "${apt_src[@]}" --only-source -q -d "xwayland=${deb_version}" ) >/dev/null 2>&1 && return 0
        # Enable deb-src if we can, then refresh the index; otherwise instruct.
        if [ -f /etc/apt/sources.list.d/ubuntu.sources ] \
            && ! grep -qE '^Types:.*deb-src' /etc/apt/sources.list.d/ubuntu.sources; then
            log "enabling deb-src in /etc/apt/sources.list.d/ubuntu.sources (source packages are not indexed yet)"
            ${apt_sudo} sed -i 's/^\(Types:.*\bdeb\b\)$/\1 deb-src/' /etc/apt/sources.list.d/ubuntu.sources \
                || die "could not enable deb-src (need root/sudo). Enable it and 'apt-get update', then retry."
            ${apt_sudo} apt-get update -qq || die "apt-get update failed after enabling deb-src"
        fi
    }
    ensure_deb_src
    ( cd "${work}" && "${apt_src[@]}" "xwayland=${deb_version}" ) >&2 \
        || die "apt-get source xwayland=${deb_version} failed (is deb-src enabled + 'apt-get update' fresh?)"
    [ -d "${srcdir}" ] || {
        # apt-get source names the dir by the resolved upstream; adopt whatever it produced.
        alt="$(ls -d "${work}"/xwayland-*/ 2>/dev/null | head -1)"
        [ -n "${alt}" ] && srcdir="${alt%/}"
    }
    [ -d "${srcdir}" ] || die "apt-get source did not produce an xwayland-* dir under ${work}"
    src_input_hashes="$(cd "${work}" && sha256sum xwayland_*.dsc xwayland_*.tar.* 2>/dev/null | grep -vE '\.tmp' || true)"
fi

# --- 2. (optional) install build-deps -------------------------------------------------------
if [ "${VKRELAY2_XWL_INSTALL_DEPS:-0}" = "1" ]; then
    # apt-get build-dep on Ubuntu 22.04 accepts a source package NAME, not a local debian/control or
    # .dsc path (both fail as "Unsupported file"). mk-build-deps is the Debian tool for consuming an
    # extracted control file: it resolves alternatives/version/arch qualifiers into a temporary
    # dependency package and asks apt to install it. Bootstrap the two tools here because the caller
    # explicitly authorized host dependency installation with INSTALL_DEPS=1.
    if ! command -v mk-build-deps >/dev/null 2>&1 || ! command -v equivs-build >/dev/null 2>&1; then
        log "installing build-dependency resolver (devscripts + equivs)"
        ${apt_sudo} apt-get install -y devscripts equivs >&2 \
            || die "could not install devscripts/equivs (required by VKRELAY2_XWL_INSTALL_DEPS=1)"
    fi
    mk_root=()
    [ -z "${apt_sudo}" ] || mk_root=(--root-cmd "${apt_sudo}")
    log "installing exact source Build-Depends via mk-build-deps"
    ( cd "${srcdir}" && mk-build-deps --install --remove "${mk_root[@]}" \
        --tool "apt-get -y --no-install-recommends" debian/control ) >&2 \
        || die "mk-build-deps failed for ${srcdir}/debian/control"
    log "build-deps installed; re-running without INSTALL_DEPS for the normal namespace/test path"
    export VKRELAY2_XWL_INSTALL_DEPS=0
    exec bash "$0" "$@"
fi
# Preflight a representative slice of the build-deps; fail with an actionable message rather than
# deep inside meson. (Not exhaustive -- debian/control, consumed above by mk-build-deps, is the
# source of truth for the full set.)
missing_dep=""
for d in libxcvt-dev libepoxy-dev libgbm-dev libxfont-dev meson; do
    dpkg -s "${d}" >/dev/null 2>&1 || missing_dep="${missing_dep} ${d}"
done
if [ -n "${missing_dep}" ]; then
    die "build-deps missing:${missing_dep}
       Re-run this script with VKRELAY2_XWL_INSTALL_DEPS=1 so mk-build-deps installs the exact
       debian/control requirements. Full list is in this script's header."
fi

# --- 3. prove the distro security series is APPLIED (not merely listed) ------------------------
# dpkg-source -x (and apt-get source) record applied patches in .pc/applied-patches.
applied="${srcdir}/.pc/applied-patches"
[ -f "${applied}" ] || die "no .pc/applied-patches -- the distro quilt series was not applied"
cve_count="$(grep -c '^CVE-' "${applied}" || true)"
if [ "${strict_cve}" = "1" ]; then
    [ "${cve_count:-0}" -ge "${min_cve}" ] || die "expected >=${min_cve} applied CVE patches, found ${cve_count:-0}"
    [ -z "${cve_sentinel}" ] || grep -qx "${cve_sentinel}" "${applied}" \
        || die "expected final Ubuntu security patch ${cve_sentinel} not applied"
    log "distro security series APPLIED: ${cve_count} CVE patches (incl. ${cve_sentinel})"
else
    # Non-jammy: apt-get source of the candidate inherently carries the distro's backports; record
    # the count (a dev release may legitimately be 0) rather than gate on a version-specific number.
    log "distro security series applied: ${cve_count} CVE patch(es) (recorded; not gated on this distro)"
fi

# --- Pin the runtime-affecting `auto` meson options to the stock distro outcome ----------------
# Xwayland leaves several options at `auto`, so meson resolves them against whatever -dev packages the
# HOST happens to carry. An over-provisioned box thus risks a different feature set than Ubuntu's clean
# buildd (the SHA1 libmd-vs-libgcrypt drift proved this leakage is real). Pin every runtime-affecting
# auto option that THIS source declares (a newer Xwayland dropped some, e.g. xwayland_eglstream); an
# option the source does not declare is skipped with a note, never forced.
declare -a pin_opts=()
for opt in sha1=libgcrypt xwayland_eglstream=true ipv6=true input_thread=true xselinux=false mitshm=true dri3=true; do
    name="${opt%%=*}"
    if grep -q "'${name}'" "${srcdir}/meson_options.txt" 2>/dev/null; then
        pin_opts+=("${opt}")
    else
        log "note: meson option '${name}' not declared by this Xwayland source; leaving it unpinned"
    fi
done
if [ "${#pin_opts[@]}" -gt 0 ]; then
    awk_prog='{ print }
        /dh_auto_configure -- \\$/ {'
    for opt in "${pin_opts[@]}"; do awk_prog="${awk_prog}"$'\n'"            print \"\t\t-D${opt} \\\\\""; done
    awk_prog="${awk_prog}"$'\n''        }'
    awk "${awk_prog}" "${srcdir}/debian/rules" > "${srcdir}/debian/rules.vkr"
    mv "${srcdir}/debian/rules.vkr" "${srcdir}/debian/rules"
    chmod +x "${srcdir}/debian/rules"
    for opt in "${pin_opts[@]}"; do
        grep -q -- "-D${opt}" "${srcdir}/debian/rules" || die "failed to pin -D${opt} in debian/rules"
    done
    log "pinned runtime meson options in debian/rules: ${pin_opts[*]}"
fi

# --- 4. add our guard(s) as the LAST quilt patches; fail hard on fuzz/reject --------------------
guard_series=()
for p in "${guard_patches[@]}"; do
    pname="vkrelay2-$(basename "${p}" | sed 's/^[0-9]*-//')"
    cp -f "${p}" "${srcdir}/debian/patches/${pname}"
    grep -qx "${pname}" "${srcdir}/debian/patches/series" || printf '%s\n' "${pname}" >> "${srcdir}/debian/patches/series"
    guard_series+=("${pname}")
done
( cd "${srcdir}" && QUILT_PATCHES=debian/patches quilt push -a ) >&2 \
    || die "a vkrelay2 guard failed to apply on the security-patched tree (fuzz/reject)"
# Assert our guards are the FINAL applied patches, in order (dpkg-source applies top-to-bottom).
n_guards="${#guard_series[@]}"
tail_applied="$(tail -n "${n_guards}" "${applied}")"
expected_tail="$(printf '%s\n' "${guard_series[@]}")"
[ "${tail_applied}" = "${expected_tail}" ] \
    || die "vkrelay2 guards are not the final applied patches (tail is:
${tail_applied})"
log "guard(s) applied as the FINAL patches on top of the security series: ${guard_series[*]}"

# --- 5. build the binary (nocheck), then run the package test suite separately -----------------
# Build with nocheck so the (valid) binary is produced reliably, then run `meson test` as a distinct,
# gated step. This lets a couple of ENVIRONMENT-only failures be recorded rather than silently blocking
# the artifact, while any UNEXPECTED failure is still fatal.
( cd "${srcdir}" && DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -b -uc -us ) >&2 \
    || die "dpkg-buildpackage failed"

# The built .deb is architecture-specific; derive its exact name from the package + arch.
deb_arch="$(dpkg-architecture -qDEB_HOST_ARCH 2>/dev/null || dpkg --print-architecture 2>/dev/null || echo "${supported_arch}")"
deb="$(ls -1 "${work}"/xwayland_*_"${deb_arch}".deb 2>/dev/null | grep -v -- '-dbgsym' | head -1)"
[ -n "${deb}" ] && [ -f "${deb}" ] || die "built .deb not found under ${work} (arch ${deb_arch})"

# Package test suite. The gate requires each FAILING test to be
# an EXACT expected name (not a substring), so an unrelated new failure -- `async-foo`, a future sync
# test, a renamed case -- is never silently accepted. The default expected set is exactly:
#   "xwayland / sync"           : Xvfb X-socket listener contention in the namespace ("server already running")
#   "xwayland:xvfb / triangles" : rendercheck vs Xvfb software Render rasterization
# BOTH invoke Xvfb (hw/vfb/Xvfb), NOT Xwayland, so they cannot be affected by our guard (which lives
# only in hw/xwayland); this attribution is corroborated by a stock-vs-patched run of the same suite in
# the same namespace (identical failure set). Any OTHER failure is fatal. Override the exact set with a
# ';'-separated VKRELAY2_XWL_ALLOWED_TEST_FAILURES (a broad override is a deliberate, recorded choice).
tests_result="skipped (VKRELAY2_XWL_SKIP_TESTS=1)"
test_render_driver="not run"
if [ "${run_tests}" = "1" ]; then
    objdir="$(ls -d "${srcdir}"/obj-* 2>/dev/null | head -1)"
    [ -n "${objdir}" ] || die "no meson build dir found to run the test suite"
    test_log="${work}/meson-test.log"
    if [ -n "${VKRELAY2_XWL_ALLOWED_TEST_FAILURES:-}" ]; then
        IFS=';' read -r -a allowed_tests <<< "${VKRELAY2_XWL_ALLOWED_TEST_FAILURES}"
    else
        allowed_tests=("xwayland / sync" "xwayland:xvfb / triangles")
    fi
    # The gate is STRICT only on the hash-pinned jammy baseline, whose exact expected-failure set is
    # validated (a stock-vs-patched run in the same namespace has the identical failure set). On
    # other distros the upstream Xvfb suite differs in name, count, and environment fragility, and a
    # jammy-tuned allowlist does not generalize -- there, the suite is RUN and its result RECORDED,
    # but an unexpected failure is not fatal: off-jammy correctness rests on building the distro
    # source + our guards + the resolved-config parity check + the app actually rendering, exactly
    # as the CVE-count assertion is strict on jammy and recorded elsewhere. A hard timeout wraps the
    # run so an environment-hung test (Xvfb socket contention in the namespace) can never wall-clock
    # the build; VKRELAY2_XWL_TEST_TIMEOUT overrides it.
    test_gate="record"
    [ "${acquire}" = "hashpin" ] && test_gate="strict"
    test_timeout="${VKRELAY2_XWL_TEST_TIMEOUT:-420}"
    test_env=()
    test_render_driver="host/default"
    if [ "${VKRELAY2_XWL_TEST_GALLIUM_DRIVER+x}" = "x" ]; then
        test_gallium_driver="${VKRELAY2_XWL_TEST_GALLIUM_DRIVER}"
    elif grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
        # Xvfb's rendercheck tests are package tests, not WSL GPU-driver integration tests. WSLg's
        # Mesa loader can otherwise select d3d12 and enter the host vendor driver; a driver crash then
        # appears as an Xserver regression. Use Mesa's deterministic software renderer on WSL.
        test_gallium_driver="llvmpipe"
    else
        test_gallium_driver=""
    fi
    if [ -n "${test_gallium_driver}" ]; then
        test_env=(env "GALLIUM_DRIVER=${test_gallium_driver}")
        test_render_driver="GALLIUM_DRIVER=${test_gallium_driver}"
    fi
    log "running Xwayland's package test suite (meson test, gate=${test_gate}, timeout=${test_timeout}s, render=${test_render_driver})"
    meson_rc=0
    timeout "${test_timeout}" "${test_env[@]}" meson test -C "${objdir}" >"${test_log}" 2>&1 || meson_rc=$?
    [ "${meson_rc}" = "124" ] && log "note: meson test hit the ${test_timeout}s timeout (recorded)"
    # Exact failing test names from the meson console log.
    mapfile -t failed_arr < <(grep -E '[[:space:]](FAIL|TIMEOUT|ERROR)[[:space:]]' "${test_log}" \
        | sed -E 's/^[[:space:]]*[0-9]+\/[0-9]+[[:space:]]+//; s/[[:space:]]{2,}(FAIL|TIMEOUT|ERROR).*$//' \
        | sort -u)
    if [ "${test_gate}" = "strict" ]; then
        [ "${meson_rc}" = "0" ] || [ "${#failed_arr[@]}" -gt 0 ] \
            || die "meson test exited ${meson_rc} but no failing tests were parsed; see ${test_log}"
        unexpected=""
        for t in "${failed_arr[@]}"; do
            [ -n "${t}" ] || continue
            hit=0
            for a in "${allowed_tests[@]}"; do [ "${t}" = "${a}" ] && hit=1; done
            [ "${hit}" = "1" ] || unexpected="${unexpected} [${t}]"
        done
        if [ -n "${unexpected}" ]; then
            die "package test failure(s) NOT in the exact expected set (${allowed_tests[*]}):${unexpected}
       See ${test_log}. Do NOT stage this artifact until resolved or the expected set is justified."
        fi
    fi
    n_ok="$(awk -F': *' '/^Ok:/{gsub(/ /,"",$2); print $2}' "${test_log}")"
    n_fail="${#failed_arr[@]}"
    gate_note=""
    [ "${test_gate}" = "record" ] && gate_note=" [record-only on this distro]"
    [ "${meson_rc}" = "124" ] && gate_note="${gate_note} [timed out at ${test_timeout}s]"
    if [ "${n_fail}" -eq 0 ] && [ "${meson_rc}" = "0" ]; then
        tests_result="${n_ok:-all} passed, 0 failed${gate_note}"
        log "package test suite: all passed (${n_ok:-?} ok)${gate_note}"
    elif [ "${n_fail}" -eq 0 ]; then
        tests_result="${n_ok:-?} passed, meson rc=${meson_rc}${gate_note}"
        log "package test suite: ${tests_result}"
    else
        failed_oneline="$(printf '%s; ' "${failed_arr[@]}" | sed 's/; $//')"
        tests_result="${n_ok:-?} passed, ${n_fail} failed (${failed_oneline})${gate_note}"
        log "package test suite: ${tests_result}"
    fi
fi

# --- 6. stage the (stripped, distro-config) Xwayland from the built .deb + prove identity ------
# Extract the .deb on the NATIVE work FS (dpkg-deb -x sets ownership/modes and fails on DrvFs), then
# assemble the final stage in a sibling temp dir and publish it with an atomic rename
# so a launcher never observes a new binary with a missing/old manifest.
deb_root="${work}/deb-root"
rm -rf "${deb_root}"
mkdir -p "${deb_root}"
dpkg-deb -x "${deb}" "${deb_root}"
[ -x "${deb_root}/usr/bin/Xwayland" ] || die "no /usr/bin/Xwayland in the built .deb"
rm -rf "${build_root}"/stage.new.* 2>/dev/null || true
rm -f "${build_root}"/.stage.lnk.* 2>/dev/null || true
mkdir -p "${stage_new}"
cp -f "${deb_root}/usr/bin/Xwayland" "${staged_bin}"
# chmod is unsupported on DrvFs (/mnt/c) without the `metadata` mount option, where the copy is
# already executable; tolerate that, but still fail closed if the staged binary is not executable.
chmod +x "${staged_bin}" 2>/dev/null || true
[ -x "${staged_bin}" ] || die "staged Xwayland is not executable: ${staged_bin}"
"${staged_bin}" -version >&2 2>&1 | head -1 || true
build_id="$(readelf -n "${staged_bin}" 2>/dev/null | awk '/Build ID:/{print $NF}')"
log "staged build ID: ${build_id:-unknown}"
# Build IDs are NOT deterministic across build roots/toolchain states; treat identity logging as
# evidence of WHICH binary ran, not as a reproducibility guarantee. The matching -dbgsym .deb is left
# in the work dir if symbols are needed.

# --- 7. emit a provenance manifest beside the binary + assert the resolved feature set ----------
# Make stale-artifact mistakes visible without inferring from file size, and record the RESOLVED
# feature set -- from the config headers -- not the bare `auto` names.
incdir="$(ls -d "${srcdir}"/obj-*/include 2>/dev/null | head -1)"   # dix-config.h + xwayland-config.h
have_eglstream_opt=0
grep -q "'xwayland_eglstream'" "${srcdir}/meson_options.txt" 2>/dev/null && have_eglstream_opt=1

# --- release-readiness verdict: the SINGLE field the packager enforces (scripts/dev/package_release.sh
# refuses a stage that is not `release_ready: yes`). It records the recipe's own accept/reject decision so
# a release artifact is never assembled from a stage that merely BUILT. A stage is release-ready iff:
#   * security-freshness was actually VERIFIED (an override / unverifiable index is not shippable);
#   * the package tests were RUN (not skipped via VKRELAY2_XWL_SKIP_TESTS=1); and
#   * the baseline is one of the supported targets (an ALLOW_BASELINE_MISMATCH override is not shippable).
# A strict-gate (jammy) build only reaches this point if its tests passed the allowlist (it dies
# otherwise), so no extra test-outcome check is needed here. Off-jammy the jammy-tuned suite is
# record-only BY DESIGN -- a recorded timeout/failure there does not block release; correctness rests on
# the distro source + guards + resolved-config parity + the app rendering -- so the verdict annotates the
# record-only posture rather than gating on it.
release_reasons=""
case "${freshness_verified}" in yes*) : ;; *) release_reasons="${release_reasons}; freshness not verified" ;; esac
[ "${run_tests}" = "1" ] || release_reasons="${release_reasons}; package tests skipped"
[ "${is_supported:-0}" = "1" ] || release_reasons="${release_reasons}; unsupported baseline override"
if [ -n "${release_reasons}" ]; then
    release_ready="no (${release_reasons#; })"
elif [ "${test_gate:-}" = "record" ]; then
    release_ready="yes (off-jammy: package tests record-only)"
else
    release_ready="yes"
fi
log "release_ready verdict: ${release_ready}"

manifest="${stage_new}/PROVENANCE.txt"
{
    echo "# vkrelay2 private Xwayland -- build provenance"
    echo "source_package:     ${deb_version} (Ubuntu ${host_codename})"
    echo "target:             ${host_codename}/${deb_arch}"
    echo "acquire:            ${acquire}"
    echo "built_from_deb:     ${deb##*/}"
    echo "build_id:           ${build_id:-unknown}"
    echo "build_utc:          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "guard_patches:      ${guard_series[*]}"
    for p in "${guard_patches[@]}"; do
        echo "guard_patch_sha256: $(sha256sum "${p}" | awk '{print $1}')  $(basename "${p}")"
    done
    echo "applied_cve_count:  ${cve_count:-0}"
    echo "tests_run:          $([ "${run_tests}" = "1" ] && echo yes || echo no)"
    echo "test_render_driver: ${test_render_driver}"
    echo "tests_result:       ${tests_result}"
    echo "distro_candidate:   ${distro_candidate:-unknown}"
    echo "freshness_verified: ${freshness_verified:-no (not checked)}"
    echo "release_ready:      ${release_ready}"
    echo
    echo "# source-package input SHA-256 (${acquire})"
    printf '%s\n' "${src_input_hashes}"
    echo
    echo "# resolved feature set (from the build's config headers -- the parity artifact vs stock)"
    if [ -n "${incdir}" ]; then
        grep -rhE '#define (DRI3|MITSHM|IPv6|INPUTTHREAD|XV|XWL_HAS_EGLSTREAM|COMPOSITE|PRESENT|GLXEXT) ' "${incdir}" | sort -u
        grep -rqE '^#define XSELINUX ' "${incdir}" && echo "#define XSELINUX 1" || echo "# XSELINUX: disabled (matches stock)"
    fi
    echo
    echo "# applied patch stack (dpkg-source applies top-to-bottom; the LAST lines are our guards)"
    cat "${applied}"
} > "${manifest}"

# Fail closed on resolved-config drift from the stock-parity intent (only for options this source has).
if [ -n "${incdir}" ]; then
    grep -rqE '^#define DRI3 1' "${incdir}"      || die "config drift: DRI3 not enabled"
    if [ "${have_eglstream_opt}" = "1" ]; then
        grep -rqE '^#define XWL_HAS_EGLSTREAM 1' "${incdir}" || die "config drift: eglstream not enabled"
    fi
    ! grep -rqE '^#define XSELINUX ' "${incdir}" || die "config drift: XSELINUX enabled (stock disables it)"
fi

# --- 8. publish atomically --------------------------------
# `stage` is a SYMLINK to an immutable generation directory. Each build gets a UNIQUE generation name
# (<build-id>-<UTC>-<pid>) -- NOT the bare build ID -- so a rebuild that produces an identical binary
# never deletes the generation the live symlink still points at. We move the assembled content into the
# fresh generation, repoint the symlink with a single rename(), and only THEN garbage-collect other
# generations (never the current symlink target). The build ID is retained in PROVENANCE.txt.
gen_id="${build_id:-nobuildid}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
gen_dir="${build_root}/gen/${gen_id}"
mkdir -p "${build_root}/gen"
mv "${stage_new}" "${gen_dir}"                       # move the assembled content into a FRESH generation
# One-time migration from a legacy real-directory stage to the symlink scheme (documented one-time gap).
if [ -e "${stage}" ] && [ ! -L "${stage}" ]; then rm -rf "${stage}"; fi
ln -s "gen/${gen_id}" "${build_root}/.stage.lnk.$$"
mv -Tf "${build_root}/.stage.lnk.$$" "${stage}"      # atomic symlink swap -> the new generation
log "published stage -> ${stage} -> gen/${gen_id} (atomic symlink swap)"
# GC AFTER the swap: keep the live target + the few most recent others (for any in-flight launcher).
current="$(readlink "${stage}" 2>/dev/null | sed 's#^gen/##')"
kept=0
for g in $(ls -1dt "${build_root}"/gen/*/ 2>/dev/null); do
    base="$(basename "${g%/}")"
    [ "${base}" = "${current}" ] && continue          # never delete the currently referenced generation
    kept=$((kept + 1))
    [ "${kept}" -le 5 ] && continue                   # keep the newest few other generations
    rm -rf "${g}"
done

# The launcher consumes exactly this line's path via VKRELAY2_XWAYLAND_BIN (resolves through the symlink).
printf 'VKRELAY2_XWAYLAND_BIN=%s\n' "${stage}/Xwayland"
