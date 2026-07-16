#!/usr/bin/env bash
# vkrelay2 boundary smoke: the guest cursor reaches the worker window (the XFixes->
# Win32 reverse direction), end to end. Brings up the private rootless-Xwayland stack + a (mock-
# backend) daemon + a session, starts the real sidecar with --debug-inject (so it warps the X pointer
# into each toplevel it registers), then maps a cursor canary (vkrelay2-cursor-canary, a pure-xcb
# toplevel that XDefineCursors a KNOWN solid-color 0x336699 cursor, NO Vulkan).
#
# When the sidecar warps the pointer into the canary, the X display cursor becomes the canary's
# cursor -> an XFixes CursorNotify -> the sidecar captures the cursor image and ships it to the worker
# via SetCursor, which builds an HCURSOR. The PROOF is worker-VISIBLE + over the wire: after the
# cursor lands, the sidecar is stopped (the worker serves one sidecar connection at a time) and
# vkrelay2-cursor-query issues DebugCursorState for the canary's XID -- PASS iff the worker reports a
# built cursor whose sampled pixel equals the canary color (0xFF336699). The canary does NOT advertise
# WM_DELETE_WINDOW, so the canonical close request is a no-op and it stays mapped for the query.
#
# Mock backend: WSL has no real GPU, and this proves the cursor capture/transport/build wiring, not
# GPU pixels. Skips cleanly (exit 0) when the private-display tools / built binaries / XFixes / XTEST
# are absent, so it is safe to run anywhere.
#
# Usage: bash run_cursor_smoke.sh [<build-dir>] [--save-png <dir>]
#   --save-png <dir>: after the worker-visible query gate, ALSO save a PNG of the captured cursor via
#   vkrelay2-capture and assert it is a valid PNG containing the canary color. The PNG is an
#   artifact; the DebugCursorState query stays the hard gate.   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
# Parse a COPY of the args (leave "$@" intact for the namespace re-exec, which re-runs this script).
build_dir=""
save_png=""
_args=("$@")
_i=0
while [ ${_i} -lt ${#_args[@]} ]; do
    case "${_args[${_i}]}" in
        --save-png) _i=$((_i + 1)); save_png="${_args[${_i}]:-}" ;;
        *) [ -z "${build_dir}" ] && build_dir="${_args[${_i}]}" ;;
    esac
    _i=$((_i + 1))
done
build_dir="${build_dir:-${script_dir}/../../build/linux-debug}"

skip() { echo "CURSOR-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-cursor-canary"
query="${build_dir}/vkrelay2-cursor-query"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}" "${query}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The canary's cursor foreground is 0x336699; the worker's built BGRA samples to this.
EXPECT_BGRA="0xFF336699"

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

vkr_start_private_display || { echo "CURSOR-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "CURSOR-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "CURSOR-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id cursor-smoke)" \
    || { echo "CURSOR-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "CURSOR-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

# --debug-inject so the sidecar warps the pointer into the canary (triggering the cursor change).
vkr_start_sidecar_and_wait_ready "${sidecar}" --debug-inject || {
    echo "CURSOR-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
# Need BOTH cursor capture (XFixes) AND input injection (XTEST, for the warp), else nothing to prove.
if ! grep -q "cursor capture enabled" "${sidecar_log}" 2>/dev/null; then
    skip "cursor capture not enabled (no libxcb-xfixes / no XFixes extension)"
fi
if ! grep -q "input injection enabled" "${sidecar_log}" 2>/dev/null; then
    skip "input injection not enabled (no libxcb-xtest / no XTEST extension; cannot warp the pointer)"
fi

# Map the cursor canary; learn its XID from its stdout.
canary_log="${VKR_RUNTIME_DIR}/cursor_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^CURSOR-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "CURSOR-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "CURSOR-SMOKE: canary mapped, xid=${XID}; waiting for the sidecar to capture + ship its cursor"

# Wait (synchronization, NOT the proof) for the sidecar to ship a successful cursor for the canary.
cursor_ok=""
for _ in $(seq 1 100); do
    grep -q "set_cursor xid=${XID} .* applied=1" "${sidecar_log}" 2>/dev/null && { cursor_ok=1; break; }
    sleep 0.1
done
[ -n "${cursor_ok}" ] || {
    echo "CURSOR-SMOKE: FAIL (no successful set_cursor for xid=${XID})"
    grep -E "set_cursor|cursor capture" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
grep -E "set_cursor xid=${XID} " "${sidecar_log}" 2>/dev/null | tail -1 | sed 's/^/    /'

# The worker serves one sidecar connection at a time, so stop the capturing sidecar to free the plane,
# then prove the worker's built cursor over the wire (the worker-visible structured query).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
echo "CURSOR-SMOKE: querying the worker (DebugCursorState) for xid=${XID}"
if ! "${query}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
        --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" --sample-x 5 --sample-y 5 \
        --expect-bgra "${EXPECT_BGRA}" 2>&1 | sed 's/^/    /'; then
    echo "CURSOR-SMOKE: FAIL (worker did not build the captured cursor)"
    exit 1
fi

# Optional artifact: save a PNG of the captured cursor from the worker's own buffer (a
# sequential sidecar-plane connection after the query). The PNG is evidence; the query is the gate.
if [ -n "${save_png}" ]; then
    capture="${build_dir}/vkrelay2-capture"
    # --save-png is an EXPLICIT request to prove the PNG path, so a missing tool is a FAILURE, not a
    # skip.
    if [ ! -x "${capture}" ]; then
        echo "CURSOR-SMOKE: FAIL (--save-png requested but vkrelay2-capture is missing: ${capture})"
        exit 1
    else
        mkdir -p "${save_png}"
        echo "CURSOR-SMOKE: saving PNG via vkrelay2-capture (xid=${XID}, layer=cursor)"
        if ! "${capture}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
                --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" --layer cursor \
                --out "${save_png}/cursor_${XID}" --expect-bgra "${EXPECT_BGRA}" 2>&1 | sed 's/^/    /'; then
            echo "CURSOR-SMOKE: FAIL (vkrelay2-capture did not produce a valid PNG with the canary color)"
            exit 1
        fi
        echo "CURSOR-SMOKE: PNG saved -> ${save_png}/cursor_${XID}.png (+ .json)"
    fi
fi

echo "============================================================"
echo "CURSOR-SMOKE: PASS (sidecar XFixes-captured the canary cursor; the worker built it;"
echo "              DebugCursorState sampled the cursor color ${EXPECT_BGRA})"
echo "============================================================"
exit 0
