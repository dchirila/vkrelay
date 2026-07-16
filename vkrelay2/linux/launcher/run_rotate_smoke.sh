#!/usr/bin/env bash
# vkrelay2 rotate-crash smoke: reproduce (and, once fixed, gate) the guest-Xwayland
# null-deref on a pointer-grab teardown -- Blender's view-rotate gesture, with NO Blender and NO
# human. Brings up the private weston-headless + rootless-Xwayland stack (the SAME bring-up an
# app-run uses) and runs vkrelay2-rotate-canary, which grabs the pointer, warp-recenters, then
# ungrabs, and reports whether Xwayland survived.
#
# Exit 0 == Xwayland SURVIVED the gesture (the FIXED state / regression-gate green).
# Exit 1 == the crash REPRODUCED (X connection lost) OR Xwayland died -- the failing baseline.
#
# Two semantics: by default a missing prerequisite SKIPs (exit 0) for broad
# developer portability. VKRELAY2_ROTATE_SMOKE_REQUIRE=1 makes it a RELEASE GATE instead: a missing
# weston/Xwayland/canary is a hard FAIL (exit 2), and only the explicit SURVIVED marker passes.
#
# Env passthrough for isolation once it reproduces (see rotate_canary.cpp):
#   ROTATE_CANARY_NO_GRAB=1 / ROTATE_CANARY_NO_LOOP_WARP=1 / ROTATE_CANARY_NO_CENTER_WARP=1 /
#   ROTATE_CANARY_WARPS=N
#
# Usage: bash run_rotate_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

require="${VKRELAY2_ROTATE_SMOKE_REQUIRE:-0}"
# In require mode a missing prerequisite is a hard failure (a release gate must not silently pass);
# otherwise it is a clean skip (portable developer run).
absent() {
    if [ "${require}" = "1" ]; then
        echo "ROTATE-SMOKE: FAIL (require mode: $1)"
        exit 2
    fi
    echo "ROTATE-SMOKE: SKIP ($1)"
    exit 0
}

command -v weston >/dev/null 2>&1 || absent "weston not found (need WSL private-display tools)"
# Honor the explicit private-binary override so a release gate points at the exact
# Xwayland under test rather than whatever PATH resolves.
if [ -n "${VKRELAY2_XWAYLAND_BIN:-}" ]; then
    [ -x "${VKRELAY2_XWAYLAND_BIN}" ] || absent "VKRELAY2_XWAYLAND_BIN not executable: ${VKRELAY2_XWAYLAND_BIN}"
else
    command -v Xwayland >/dev/null 2>&1 || absent "Xwayland not found"
fi
canary="${build_dir}/vkrelay2-rotate-canary"
[ -x "${canary}" ] || absent "missing binary ${canary} (build the linux preset first)"

# shellcheck source=lib_private_session.sh
. "${script_dir}/lib_private_session.sh"

CANARY_PID=""
smoke_cleanup() {
    local ec="${1:-0}"
    [ -n "${CANARY_PID}" ] && kill "${CANARY_PID}" 2>/dev/null || true
    vkr_session_cleanup "${ec}"
}
trap 'smoke_cleanup "$?"' EXIT

vkr_reexec_in_private_x11_namespace "$@"

vkr_start_private_display || { echo "ROTATE-SMOKE: FAIL (private display setup)"; exit 1; }

# Prove the seat-less precondition (the root cause) so the log records it either way.
if command -v weston-info >/dev/null 2>&1; then
    if WAYLAND_DISPLAY="${WAYLAND_DISPLAY}" weston-info 2>/dev/null | grep -q "wl_seat"; then
        echo "ROTATE-SMOKE: note: compositor advertises wl_seat (seat-less precondition absent)"
    else
        echo "ROTATE-SMOKE: note: compositor advertises NO wl_seat (seat-less precondition present)"
    fi
fi

echo "ROTATE-SMOKE: running the rotate canary on DISPLAY=${DISPLAY}"
out="$("${canary}" 2>&1)"; rc=$?
echo "${out}" | sed 's/^/    /'

# Corroborate with the Xwayland process state: a crash SIGSEGVs the server.
xwayland_dead=""
if [ -n "${VKR_XWAYLAND_PID:-}" ] && ! kill -0 "${VKR_XWAYLAND_PID}" 2>/dev/null; then
    xwayland_dead=1
fi

if [ "${rc}" -eq 0 ] && [ -z "${xwayland_dead}" ]; then
    echo "============================================================"
    echo "ROTATE-SMOKE: PASS (Xwayland SURVIVED grab+recenter+ungrab)"
    echo "============================================================"
    exit 0
fi

echo "============================================================"
echo "ROTATE-SMOKE: REPRODUCED (canary rc=${rc}, xwayland_dead=${xwayland_dead:-0})"
echo "  Xwayland backtrace (if any):"
grep -E "Segmentation|Backtrace|EE|signal" "${VKR_RUNTIME_DIR}/xwayland.log" 2>/dev/null | sed 's/^/    /' || true
echo "============================================================"
exit 1
