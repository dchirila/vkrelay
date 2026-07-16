#!/usr/bin/env bash
# vkrelay2 boundary smoke: the REAL sidecar binary, end to end through the production
# launcher path, on our private rootless-Xwayland stack. Brings up the private display, starts a
# (mock-backend) daemon, opens a session, and then calls the SAME launcher helper the app-run path
# uses -- vkr_start_sidecar_and_wait_ready -- so the sidecar (vkrelay2-sidecar) connects to the
# worker's sidecar plane, claims the WM on the private display, probes caps, scans, emits
# sidecar_ready, and raises its --ready-fd edge. PASS = that helper returns success (the readiness
# barrier fired) within its bounded wait.
#
# Mock backend: WSL has no real GPU, and this proves the sidecar/WM/readiness wiring, not on-screen
# pixels (that is run_canary / run_vkcube). Skips cleanly (exit 0) when the private-display tools or
# the built binaries are absent (e.g. on a non-WSL box), so it is safe to run anywhere.
#
# Usage: bash run_sidecar_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "SIDECAR-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
probe="${build_dir}/vkrelay2-configure-probe"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${probe}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# Reuse the production helpers: the private-display bring-up, the session cleanup, AND the exact
# sidecar-start-and-wait-ready function the app-run path calls.
# shellcheck source=lib_private_session.sh
. "${script_dir}/lib_private_session.sh"

DAEMON_PID=""
smoke_cleanup() {
    local ec="${1:-0}"
    [ -n "${DAEMON_PID}" ] && kill "${DAEMON_PID}" 2>/dev/null || true
    vkr_session_cleanup "${ec}"
}
trap 'smoke_cleanup "$?"' EXIT

# WSLg read-only /tmp/.X11-unix workaround (same as the app-run path).
vkr_reexec_in_private_x11_namespace "$@"

vkr_start_private_display || { echo "SIDECAR-SMOKE: FAIL (private display setup)"; exit 1; }

# Start a mock-backend daemon and learn its port from the port-file.
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
[ -n "${ok}" ] || { echo "SIDECAR-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "SIDECAR-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

# Open a session (mock daemon -> no --require-worker-backend real) and export the session env,
# including VKRELAY2_SIDECAR_PLANE_* / VKRELAY2_SIDECAR_TOKEN.
session_env="$("${launch}" --open-session --app-id sidecar-smoke)" \
    || { echo "SIDECAR-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "SIDECAR-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }
echo "SIDECAR-SMOKE: session sidecar plane on ${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"

# The crux: the production launcher helper starts the real sidecar binary and blocks on its
# readiness edge.
vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "SIDECAR-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

# The sidecar is the WM now. Verify it passes a client ConfigureRequest THROUGH (move + resize):
# a WM that holds SubstructureRedirect but drops ConfigureRequest would silently deny ordinary
# toolkit placement/resize once it starts.
echo "SIDECAR-SMOKE: checking ConfigureRequest pass-through under the sidecar WM"
if ! DISPLAY="${DISPLAY}" "${probe}" >"${VKR_RUNTIME_DIR}/configure_probe.log" 2>&1; then
    echo "SIDECAR-SMOKE: FAIL (configure pass-through)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/configure_probe.log" 2>/dev/null || true
    exit 1
fi
sed 's/^/    /' "${VKR_RUNTIME_DIR}/configure_probe.log" 2>/dev/null || true

# The configure_probe maps a window, configures it, then exits -- so it also exercised the
# lifecycle forwarding end to end: the sidecar should have forwarded register_toplevel (on the
# probe's MapRequest) and, when the probe window went away, unregister_toplevel (on UnmapNotify/
# DestroyNotify) to the worker registry. Assert both fired in the sidecar log (the unregister lands
# asynchronously after the probe disconnects, so allow a brief settle).
echo "SIDECAR-SMOKE: checking toplevel register/unregister forwarding"
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
reg_ok="" unreg_ok=""
for _ in $(seq 1 30); do
    grep -q "register_toplevel xid=" "${sidecar_log}" 2>/dev/null && reg_ok=1
    grep -q "unregister_toplevel xid=" "${sidecar_log}" 2>/dev/null && unreg_ok=1
    [ -n "${reg_ok}" ] && [ -n "${unreg_ok}" ] && break
    sleep 0.1
done
if [ -z "${reg_ok}" ] || [ -z "${unreg_ok}" ]; then
    echo "SIDECAR-SMOKE: FAIL (lifecycle forwarding: register=${reg_ok:-0} unregister=${unreg_ok:-0})"
    grep -E "register_toplevel|unregister_toplevel" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
fi
grep -E "register_toplevel|unregister_toplevel" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true

echo "============================================================"
echo "SIDECAR-SMOKE: PASS (sidecar claimed the WM, emitted sidecar_ready, raised --ready-fd;"
echo "               ConfigureRequest honored; register/unregister forwarded to the worker)"
echo "============================================================"
exit 0
