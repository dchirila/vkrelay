#!/usr/bin/env bash
# vkrelay2 boundary smoke: Win32-USER-origin geometry feedback over the wire (the
# REVERSE direction, worker->sidecar). Brings up the private rootless-Xwayland stack + a (mock-backend)
# daemon + a session, starts the real sidecar with --debug-feedback-move X,Y,W,H (which seeds a
# Win32-user-origin GeometryRequest -- a move+RESIZE -- into the worker ring for the first toplevel,
# standing in for a real caption drag / border resize, which cannot originate on WSL), then maps
# vkrelay2-feedback-canary.
#
# The PROOF is over the wire + worker-VISIBLE-by-effect: the sidecar drains the GeometryRequest
# (PollInput) and AUTHORS the GUEST move+resize (xcb_configure_window X|Y|WIDTH|HEIGHT); the canary
# observes its own ConfigureNotify and prints the realized position AND size. The smoke asserts the
# guest converged to the requested X,Y and W,H -- i.e. a worker-origin host gesture became a guest
# geometry change (the resize half rides user_request_is_resize -> the
# extent_authoritative path), the "Win32 events are requests" loop closed. (The worker leg --
# WM_EXITSIZEMOVE -> the enqueued GeometryRequest with the inverse-mapped coords + committed extent --
# is proven in-process by the Windows integration_real_backend test_real_user_geometry_feedback; this
# smoke proves the sidecar leg over the wire.)
#
# Mock backend: WSL has no GPU; the mock's input ring + PollInput carry the GeometryRequest exactly
# like the real worker (mock == real for the transport), and the sidecar authoring is real X. Skips
# cleanly (exit 0) when the private-display tools / built binaries are absent.
#
# Usage: bash run_feedback_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "FEEDBACK-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-feedback-canary"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The requested target the sidecar seeds (a Win32-user-origin move+RESIZE); the canary starts at
# 50,60 and 300x220. A NEW extent (different from 300x220) makes the request a real resize,
# so the sidecar authors XCB_CONFIG_WINDOW_WIDTH|HEIGHT and the guest converges to it.
TARGET_X=260
TARGET_Y=180
TARGET_W=420
TARGET_H=300

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

vkr_start_private_display || { echo "FEEDBACK-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "FEEDBACK-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "FEEDBACK-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id feedback-smoke)" \
    || { echo "FEEDBACK-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "FEEDBACK-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

# The sidecar seeds a Win32-user-origin GeometryRequest (move+resize to TARGET) for the first
# toplevel. The 4-field form "X,Y,W,H" requests a resize (vs the 2-field "X,Y" move).
vkr_start_sidecar_and_wait_ready "${sidecar}" \
    --debug-feedback-move "${TARGET_X},${TARGET_Y},${TARGET_W},${TARGET_H}" || {
    echo "FEEDBACK-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

canary_log="${VKR_RUNTIME_DIR}/feedback_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^FEEDBACK-CANARY: xid=\([0-9]\+\) .*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "FEEDBACK-SMOKE: FAIL (canary did not map)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "FEEDBACK-SMOKE: canary xid=${XID} mapped; expecting the guest to follow the user request -> ${TARGET_X},${TARGET_Y} ${TARGET_W}x${TARGET_H}"

# The PROOF: the sidecar authored the guest move+RESIZE from the worker-origin GeometryRequest -> the
# canary observes its ConfigureNotify at the requested position AND extent (the WIDTH|HEIGHT
# half rides through user_request_is_resize -> the extent_authoritative path).
moved=""
for _ in $(seq 1 100); do
    grep -qE "^FEEDBACK-CANARY: configure=${TARGET_X},${TARGET_Y} size=${TARGET_W},${TARGET_H}$" "${canary_log}" 2>/dev/null && { moved=1; break; }
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${moved}" ] || {
    echo "FEEDBACK-SMOKE: FAIL (guest did not converge to ${TARGET_X},${TARGET_Y} ${TARGET_W}x${TARGET_H})"
    grep -E "FEEDBACK-CANARY:" "${canary_log}" 2>/dev/null | sed 's/^/    /'
    grep -E "user_geometry|debug-feedback" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null | sed 's/^/    sidecar: /' || true
    exit 1
}
echo "FEEDBACK-SMOKE: PASS (the guest followed the Win32-user-origin request to ${TARGET_X},${TARGET_Y} and resized to ${TARGET_W}x${TARGET_H})"
