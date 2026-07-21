#!/usr/bin/env bash
# vkrelay2 boundary smoke: DEFAULT CENTERING of an unpositioned toplevel, end to end. Same
# bring-up as run_geometry_smoke (private rootless-Xwayland + a mock-backend daemon + a session + the
# real sidecar/WM), then maps vkrelay2-placement-canary: a normal toplevel created at the origin with
# NO position hint (no WM_NORMAL_HINTS USPosition/PPosition -- the common Qt/GTK/Tk default, NOT
# glxgears, which declares USPosition at 0,0 and is honored). must CENTER an unpositioned window
# on the X root before mapping (configuring the X window's position), so it does NOT appear with its
# chrome off the top of the screen.
#
# The PROOF is worker-VISIBLE and over the wire: the sidecar logs the centered origin it chose; the
# smoke asserts that origin is NON-zero (it actually centered) and then -- after stopping the sidecar to
# free the worker's single sidecar connection -- uses vkrelay2-geometry-query (DebugEnumWindows
# include_actual) to assert the ACTUAL host CLIENT origin converged to exactly that centered origin
# (has_actual + last_host_apply_seq != 0). Older code placed the window at 0,0 and fails this check.
#
# Mock backend (WSL has no real GPU): proves the centering decision + transport + placement (mock ==
# real for the authored origin); the worker's on-screen clamp is unit-tested separately. Skips cleanly
# (exit 0) when the private-display tools / built binaries are absent.
#
# Usage: bash run_placement_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "PLACEMENT-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-placement-canary"
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

vkr_reexec_in_private_x11_namespace "$@" || exit $?

vkr_start_private_display || { echo "PLACEMENT-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "PLACEMENT-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "PLACEMENT-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id placement-smoke)" \
    || { echo "PLACEMENT-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "PLACEMENT-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "PLACEMENT-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"

# Map the placement canary (NO position hint); learn its xid from its stdout.
canary_log="${VKR_RUNTIME_DIR}/placement_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^PLACEMENT-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "PLACEMENT-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }

# The sidecar logs the centered origin it chose for this xid (synchronization + the chosen target).
CENTER=""
for _ in $(seq 1 100); do
    CENTER="$(sed -n "s/.*centered toplevel xid=${XID} [0-9]*x[0-9]* -> \([0-9]*\),\([0-9]*\).*/\1 \2/p" "${sidecar_log}" 2>/dev/null | head -1)"
    [ -n "${CENTER}" ] && break
    sleep 0.1
done
[ -n "${CENTER}" ] || {
    echo "PLACEMENT-SMOKE: FAIL (sidecar did not center xid ${XID} -- no 'centered toplevel' log)"
    grep -E "register_toplevel|centered toplevel" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
CX="${CENTER%% *}"
CY="${CENTER##* }"
echo "PLACEMENT-SMOKE: sidecar centered xid=${XID} -> ${CX},${CY}"
# It must NOT be the requested origin -- that would mean it was never centered.
if [ "${CX}" -eq 0 ] && [ "${CY}" -eq 0 ]; then
    echo "PLACEMENT-SMOKE: FAIL (centered origin is 0,0 -- no centering happened)"
    exit 1
fi

# The worker serves one sidecar connection at a time: stop the sidecar, then prove over the wire that
# the ACTUAL host client origin converged to the centered origin (initial placement applied).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "PLACEMENT-SMOKE: querying the worker (DebugEnumWindows include_actual) for xid=${XID} -> ${CX},${CY}"
if ! "${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${XID}" --expect-x "${CX}" --expect-y "${CY}" 2>&1 | sed 's/^/    /'; then
    echo "PLACEMENT-SMOKE: FAIL (worker did not place the unpositioned toplevel at the centered origin)"
    exit 1
fi

echo "============================================================"
echo "PLACEMENT-SMOKE: PASS (an unpositioned toplevel was centered to ${CX},${CY} and the worker placed"
echo "                 the host there -- no more chrome-off-screen at 0,0)"
echo "============================================================"
exit 0
