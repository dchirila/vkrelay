#!/usr/bin/env bash
# vkrelay2 boundary smoke: a GUEST-driven RESIZE tracks to the worker host, end to end. Same
# bring-up as run_placement_smoke (private rootless-Xwayland + a mock-backend daemon + a session + the
# real sidecar/WM), then maps vkrelay2-resize-canary: a positioned toplevel created at size A that, once
# mapped, issues its OWN ConfigureRequest RESIZE to size B. must forward that resize to the worker
# (sidecar ConfigureRequest -> forward_update -> registry size_changed -> apply_size -> the host client
# resizes), so the host client must end up at B -- NOT stuck at A.
#
# The PROOF is worker-VISIBLE and over the wire: after the sidecar logs the forwarded update_toplevel
# for the canary's xid, the smoke stops the sidecar (to free the worker's single sidecar connection) and
# uses vkrelay2-geometry-query (DebugEnumWindows include_actual) to assert the ACTUAL host CLIENT extent
# == B at the canary's (honored, PPosition) origin. The in-process integration_real_backend test drives
# the registry directly and so cannot cover this sidecar ConfigureRequest -> forward_update translation.
#
# Mock backend (WSL has no real GPU): the canary is a placeholder (no Vulkan surface), which is exactly
# right here -- the path under test is the sidecar's guest-resize forwarding + the worker's host client
# resize (apply_size), independent of GL. Skips cleanly (exit 0) when tools / built binaries are absent.
#
# Usage: bash run_resize_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

# The canary's chosen geometry (must match resize_canary.cpp): position is HONORED (PPosition), size A
# -> B via the guest ConfigureRequest.
kX=120
kY=90
kBW=360
kBH=260

skip() { echo "RESIZE-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-resize-canary"
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

vkr_start_private_display || { echo "RESIZE-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "RESIZE-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "RESIZE-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id resize-smoke)" \
    || { echo "RESIZE-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "RESIZE-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "RESIZE-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"

# Map the resize canary; learn its xid; wait for it to issue the resize.
canary_log="${VKR_RUNTIME_DIR}/resize_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^RESIZE-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "RESIZE-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }

# Wait for the sidecar to FORWARD the resize as an update_toplevel for this xid (the guest
# ConfigureRequest -> forward_update path). The canary issues the resize after the WM maps it; the
# sidecar logs "update_toplevel xid=<XID> gen=..." for the forwarded size change.
got_update=""
for _ in $(seq 1 100); do
    if grep -q "update_toplevel xid=${XID} " "${sidecar_log}" 2>/dev/null; then
        got_update=1
        break
    fi
    sleep 0.1
done
[ -n "${got_update}" ] || {
    echo "RESIZE-SMOKE: FAIL (sidecar never forwarded a resize update_toplevel for xid ${XID})"
    grep -E "register_toplevel|update_toplevel" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
echo "RESIZE-SMOKE: sidecar forwarded the guest resize for xid=${XID}"

# The worker serves one sidecar connection at a time: stop the sidecar, then prove over the wire that
# the ACTUAL host client converged to B at the honored origin (the resize tracked).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "RESIZE-SMOKE: querying the worker (DebugEnumWindows include_actual) for xid=${XID} -> ${kX},${kY} @ ${kBW}x${kBH}"
if ! "${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${XID}" --expect-x "${kX}" --expect-y "${kY}" --expect-w "${kBW}" --expect-h "${kBH}" \
        2>&1 | sed 's/^/    /'; then
    echo "RESIZE-SMOKE: FAIL (worker host client did not converge to the guest-requested resize ${kBW}x${kBH})"
    exit 1
fi

echo "============================================================"
echo "RESIZE-SMOKE: PASS (a guest ConfigureRequest resize tracked to the worker host client ${kBW}x${kBH}"
echo "              at ${kX},${kY} -- the sidecar ConfigureRequest -> forward_update -> host resize path)"
echo "============================================================"
exit 0
