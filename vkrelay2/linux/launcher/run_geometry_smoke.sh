#!/usr/bin/env bash
# vkrelay2 boundary smoke: LIVE host MOVE driven by sidecar geometry authority, end
# to end. Brings up the private rootless-Xwayland stack + a (mock-backend) daemon + a session, starts
# the real sidecar (WM), then maps vkrelay2-geometry-canary: a normal toplevel that MOVES ITSELF
# across a known sequence of X-root positions via xcb_configure_window. Under the sidecar WM each
# self-move arrives as a ConfigureRequest the sidecar honors + forwards as an UpdateToplevel; from
# a position change drives a live host move at the worker.
#
# The PROOF is worker-VISIBLE and over the wire (not a log): after the canary settles at its final
# position, the sidecar is stopped (the worker serves one sidecar connection at a time) and
# vkrelay2-geometry-query (DebugEnumWindows include_actual) asserts the toplevel's ACTUAL host CLIENT
# origin converged to the final authored position (+ has_actual + last_host_apply_seq != 0).
#
# Mock backend: WSL has no real GPU, and this proves the geometry transport + the apply decision
# (mock == real for the authored position + seq; the real on-screen move is proven by the Windows
# in-process integration_real_backend test + capture_window.ps1). Skips cleanly (exit 0) when the
# private-display tools / built binaries are absent, so it is safe to run anywhere.
#
# Usage: bash run_geometry_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "GEOMETRY-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-geometry-canary"
query="${build_dir}/vkrelay2-geometry-query"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}" "${query}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# shellcheck source=lib_private_session.sh
. "${script_dir}/lib_private_session.sh"

DAEMON_PID=""
CANARY_PID=""
smoke_cleanup() {
    local ec="${1:-0}"
    [ -n "${CANARY_PID}" ] && kill "${CANARY_PID}" 2>/dev/null || true
    [ -n "${DAEMON_PID}" ] && kill "${DAEMON_PID}" 2>/dev/null || true
    vkr_session_cleanup "${ec}"
}
trap 'smoke_cleanup "$?"' EXIT

vkr_reexec_in_private_x11_namespace "$@"

vkr_start_private_display || { echo "GEOMETRY-SMOKE: FAIL (private display setup)"; exit 1; }

port_file="${VKR_RUNTIME_DIR}/daemon.port"
"${supervisor}" --serve --port 0 --port-file "${port_file}" --worker "${worker}" \
    --vulkan-backend mock >"${VKR_RUNTIME_DIR}/daemon.log" 2>&1 &
DAEMON_PID=$!
ok=""
for _ in $(seq 1 50); do
    [ -s "${port_file}" ] && { ok=1; break; }
    kill -0 "${DAEMON_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${ok}" ] || { echo "GEOMETRY-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "GEOMETRY-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id geometry-smoke)" \
    || { echo "GEOMETRY-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "GEOMETRY-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "GEOMETRY-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"

# Map the geometry canary; learn its xid + final authored position from its stdout.
canary_log="${VKR_RUNTIME_DIR}/geometry_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
STATIC_XID="" STATIC_POS="" MOVER_XID="" MOVER_FINAL="" MOVER_SIZE=""
for _ in $(seq 1 50); do
    STATIC_XID="$(sed -n 's/^GEOMETRY-CANARY: static=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    STATIC_POS="$(sed -n 's/^GEOMETRY-CANARY: .*static_pos=\([0-9]\+,[0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    MOVER_XID="$(sed -n 's/^GEOMETRY-CANARY: .*mover=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    MOVER_FINAL="$(sed -n 's/^GEOMETRY-CANARY: .*mover_final=\([0-9]\+,[0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    MOVER_SIZE="$(sed -n 's/^GEOMETRY-CANARY: .*mover_size=\([0-9]\+,[0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${STATIC_XID}" ] && [ -n "${STATIC_POS}" ] && [ -n "${MOVER_XID}" ] && [ -n "${MOVER_FINAL}" ] && [ -n "${MOVER_SIZE}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${STATIC_XID}" ] && [ -n "${STATIC_POS}" ] && [ -n "${MOVER_XID}" ] && [ -n "${MOVER_FINAL}" ] && [ -n "${MOVER_SIZE}" ] \
    || { echo "GEOMETRY-SMOKE: FAIL (canary did not report its xids/positions)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
STATIC_X="${STATIC_POS%,*}"
STATIC_Y="${STATIC_POS#*,}"
MOVER_X="${MOVER_FINAL%,*}"
MOVER_Y="${MOVER_FINAL#*,}"
MOVER_W="${MOVER_SIZE%,*}"
MOVER_H="${MOVER_SIZE#*,}"
echo "GEOMETRY-SMOKE: canary mapped, static=${STATIC_XID}@${STATIC_X},${STATIC_Y} mover=${MOVER_XID}->${MOVER_X},${MOVER_Y}@${MOVER_W}x${MOVER_H}; waiting for the sidecar to forward the raise"

# Wait (synchronization, NOT the proof) for the sidecar to forward the mover's FINAL self-RAISE
# (z=1) as applied -- the raise is the LAST op (after the moves + resize), so this implies the whole
# move/resize/restack sequence reached the worker, and the static window is registered (initial
# placement applied) by then.
reg_ok=""
for _ in $(seq 1 100); do
    grep -qE "update_toplevel xid=${MOVER_XID} .* z=1 applied=1" "${sidecar_log}" 2>/dev/null && { reg_ok=1; break; }
    sleep 0.1
done
[ -n "${reg_ok}" ] || {
    echo "GEOMETRY-SMOKE: FAIL (sidecar did not forward the final raise for xid ${MOVER_XID})"
    grep -E "update_toplevel xid=${MOVER_XID}" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
grep -E "update_toplevel xid=${MOVER_XID} .* z=1 " "${sidecar_log}" 2>/dev/null | tail -1 | sed 's/^/    /'

# The worker serves one sidecar connection at a time, so stop the sidecar to free the plane, then
# prove convergence over the wire (the worker-visible structured query) for BOTH windows:
#   - the MOVER converged to its final self-move POSITION + self-resize EXTENT + self-RAISE (z=1)
#     (live move + resize + restack), and
#   - the STATIC window converged to its map position with NO self-move (INITIAL PLACEMENT --
#     the vkcube/OpenSCAD class that maps once and never moves).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "GEOMETRY-SMOKE: querying the worker (DebugEnumWindows include_actual) for mover=${MOVER_XID} -> ${MOVER_X},${MOVER_Y} @ ${MOVER_W}x${MOVER_H} z=1"
if ! "${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${MOVER_XID}" --expect-x "${MOVER_X}" --expect-y "${MOVER_Y}" \
        --expect-w "${MOVER_W}" --expect-h "${MOVER_H}" --expect-z 1 2>&1 | sed 's/^/    /'; then
    echo "GEOMETRY-SMOKE: FAIL (worker did not converge the mover to the authored geometry + raise)"
    exit 1
fi
echo "GEOMETRY-SMOKE: querying the worker for static=${STATIC_XID} -> ${STATIC_X},${STATIC_Y} (initial placement, no self-move)"
if ! "${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${STATIC_XID}" --expect-x "${STATIC_X}" --expect-y "${STATIC_Y}" 2>&1 | sed 's/^/    /'; then
    echo "GEOMETRY-SMOKE: FAIL (worker did not place the static window at its map position)"
    exit 1
fi

echo "============================================================"
echo "GEOMETRY-SMOKE: PASS (the worker converged the mover to ${MOVER_X},${MOVER_Y} @ ${MOVER_W}x${MOVER_H} (raised)"
echo "             AND placed the never-moved static window at ${STATIC_X},${STATIC_Y} -- move + resize + raise + initial placement)"
echo "============================================================"
exit 0
