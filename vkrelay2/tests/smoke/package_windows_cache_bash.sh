#!/usr/bin/env bash
# Cache-owner/removal contract tests that do not touch the real Windows LocalAppData tree.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
remove_helper="${repo}/scripts/packaging/remove_windows_payload.sh"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
root="${work}/local-app-data"
record="${work}/cache-root-record"
package_id="0.0.0+cache-contract-test"
cache="${root}/vkrelay2/${package_id}"
owner="${cache}.vkrelay2-package"

run_remove() {
    VKRELAY2_WINDOWS_CACHE_ROOT="${root}" \
    VKRELAY2_WINDOWS_CACHE_ROOT_RECORD="${record}" \
        bash "${remove_helper}" "${package_id}"
}

# Interrupted old materialization: lock/known partial files but no marker. This must be recoverable,
# not a permanent dpkg wedge.
mkdir -p "${cache}"
: > "${cache}/.install.lock"
: > "${cache}/.vkrelay2-worker.exe.tmp.123"
run_remove
[[ ! -e "${cache}" && ! -e "${owner}" ]]
echo "ok: unmarked known-incomplete cache removed"

# New creator contract: ownership may be published immediately before mkdir. A marker-only
# interruption must cleanly disappear.
mkdir -p "$(dirname "${owner}")"
printf '%s\n' "${package_id}" > "${owner}"
run_remove
[[ ! -e "${owner}" ]]
echo "ok: marker-only interrupted creation removed"

# Once ownership is proven, removal owns every cache entry and removes the marker only after the
# directory itself is gone.
mkdir -p "${cache}"
printf '%s\n' "${package_id}" > "${owner}"
printf 'payload\n' > "${cache}/unexpected-but-owned-file"
run_remove
[[ ! -e "${cache}" && ! -e "${owner}" ]]
echo "ok: marked cache and sibling ownership marker removed"

# An unknown unmarked directory is never adopted or recursively removed.
mkdir -p "${cache}"
printf 'keep\n' > "${cache}/user-file"
if run_remove >/dev/null 2>&1; then
    echo "ERROR: unknown unmarked cache was accepted" >&2
    exit 1
fi
[[ -r "${cache}/user-file" ]]
echo "ok: unknown unmarked directory refused"

echo "PACKAGE-WINDOWS-CACHE: PASS"
