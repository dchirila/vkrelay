#!/usr/bin/env bash
# launcher regression: the private Xwayland auto-selection must accept only an exact
# provenance/host match. Pure metadata test -- no compositor, X server, package database, or GPU.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
lib="${here}/../../linux/launcher/lib_private_session.sh"
if [ ! -f "${lib}" ]; then
    echo "XWAYLAND-SELECTION: SKIP (lib not found: ${lib})"
    exit 0
fi
# shellcheck source=/dev/null
. "${lib}"

if ! command -v vkr_private_xwayland_metadata_compatible >/dev/null 2>&1; then
    echo "XWAYLAND-SELECTION: FAIL (metadata helper not defined)"
    exit 1
fi
if ! command -v vkr_stock_xwayland_known_unsafe >/dev/null 2>&1; then
    echo "XWAYLAND-SELECTION: FAIL (known-unsafe helper not defined)"
    exit 1
fi

tmp="$(mktemp -d)" || exit 1
trap 'rm -rf "${tmp}"' EXIT
manifest="${tmp}/PROVENANCE.txt"
cat >"${manifest}" <<'EOF'
# synthetic private-Xwayland provenance
source_package:     2:22.1.1-1ubuntu0.20 (Ubuntu jammy-security)
target:             jammy/amd64
build_id:           abcdef012345
freshness_verified: yes (synthetic test)
EOF

fail=0
expect_pass() {
    if vkr_private_xwayland_metadata_compatible "$@"; then
        echo "ok: exact compatible private Xwayland accepted"
    else
        echo "FAIL: exact compatible private Xwayland rejected"
        fail=1
    fi
}
expect_fail() { # <description> <args...>
    local description="$1"
    shift
    if vkr_private_xwayland_metadata_compatible "$@"; then
        echo "FAIL: ${description} accepted"
        fail=1
    else
        echo "ok: ${description} rejected"
    fi
}

expect_pass "${manifest}" jammy amd64 2:22.1.1-1ubuntu0.20 abcdef012345
expect_fail "wrong distro" "${manifest}" noble amd64 2:22.1.1-1ubuntu0.20 abcdef012345
expect_fail "wrong architecture" "${manifest}" jammy arm64 2:22.1.1-1ubuntu0.20 abcdef012345
expect_fail "different package baseline" "${manifest}" jammy amd64 2:22.1.1-1ubuntu0.21 abcdef012345
expect_fail "different ELF build ID" "${manifest}" jammy amd64 2:22.1.1-1ubuntu0.20 deadbeef

sed -i 's/freshness_verified: yes/freshness_verified: no/' "${manifest}"
expect_fail "unverified freshness" "${manifest}" jammy amd64 2:22.1.1-1ubuntu0.20 abcdef012345

# The tested distros' STOCK Xwayland is known-unsafe on the seatless compositor regardless of the
# exact version/build-id (upstream has not merged vkrelay2's guards, so every stock build crashes).
for cn in jammy noble resolute; do
    if vkr_stock_xwayland_known_unsafe "${cn}" amd64 any-version any-build-id; then
        echo "ok: stock ${cn} recognized as known-crashing"
    else
        echo "FAIL: stock ${cn} not recognized as known-crashing"
        fail=1
    fi
done
# An UNTESTED distro is not blindly guessed unsafe (an explicit VKRELAY2_XWAYLAND_BIN can still run
# a stock control there); a non-amd64 arch is likewise not covered by the tested set.
if vkr_stock_xwayland_known_unsafe someotherdistro amd64 2:99.0-1 abcd; then
    echo "FAIL: untested distro guessed unsafe"
    fail=1
else
    echo "ok: untested distro not guessed unsafe"
fi
if vkr_stock_xwayland_known_unsafe jammy arm64 2:22.1.1-1ubuntu0.20 abcd; then
    echo "FAIL: non-amd64 arch flagged unsafe"
    fail=1
else
    echo "ok: non-amd64 arch not in the tested set"
fi

if [ "${fail}" -eq 0 ]; then
    echo "XWAYLAND-SELECTION: PASS"
    exit 0
fi
echo "XWAYLAND-SELECTION: FAIL"
exit 1
