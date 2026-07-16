#!/usr/bin/env bash
# vkrelay2 boundary smoke: input/focus/wheel/close through the sidecar, end to end.
# Brings up the private rootless-Xwayland stack + a (mock-backend) daemon + a session, starts the
# real sidecar with --debug-inject (which claims the WM and enables input injection), then maps a
# neutral input canary (vkrelay2-input-canary, a pure-xcb toplevel that selects input + advertises
# WM_DELETE_WINDOW, NO Vulkan).
#
# The PROOF is the canary as a STRICT ORACLE: when the canary maps, the sidecar registers it, seeds a
# CANONICAL neutral input sequence into the worker's input ring (DebugEnqueueInput), then drains it
# over the wire (PollInput) and INJECTS it into the canary via XTest / xcb_set_input_focus /
# WM_DELETE_WINDOW. The canary passes (exit 0) ONLY after receiving exactly the focus/click/wheel/
# key/close sequence, in order -- so this proves worker-ring -> PollInput -> sidecar -> guest end to
# end. Real Win32 input cannot originate on WSL, so the worker ring is filled via the debug seam (the
# WndProc half is proven separately by the Windows in-process integration_real_backend test).
#
# Mock backend: WSL has no real GPU, and this proves the input transport/injection wiring, not GPU
# pixels. Skips cleanly (exit 0) when the private-display tools / built binaries / XTEST are absent,
# so it is safe to run anywhere.
#
# Usage: bash run_input_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "INPUT-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-input-canary"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}"; do
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

vkr_start_private_display || { echo "INPUT-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "INPUT-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "INPUT-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id input-smoke)" \
    || { echo "INPUT-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "INPUT-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

# Start the sidecar with --debug-inject so it seeds + injects the canonical sequence for each mapped
# toplevel.
vkr_start_sidecar_and_wait_ready "${sidecar}" --debug-inject || {
    echo "INPUT-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
# Input injection must be compiled in (libxcb-xtest) AND XTEST present, else there is nothing to
# prove -- skip cleanly. (Focus/close work without XTEST, but the canary requires the full sequence.)
if ! grep -q "input injection enabled" "${sidecar_log}" 2>/dev/null; then
    skip "input injection not enabled (no libxcb-xtest / no XTEST extension)"
fi

# Map the input canary (the strict oracle); it runs until it has received the full canonical sequence
# (exit 0) or its internal deadline elapses (exit 1).
canary_log="${VKR_RUNTIME_DIR}/input_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^INPUT-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "INPUT-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "INPUT-SMOKE: canary mapped, xid=${XID}; waiting for the sidecar to inject the canonical sequence"

# Wait for the oracle to finish (it self-terminates: PASS exit 0, or its own ~30s deadline exit 1).
wait "${CANARY_PID}"
rc=$?
CANARY_PID=""
sed 's/^/    /' "${canary_log}" 2>/dev/null || true
grep -E "debug-inject seeded|focus ->|close ->" "${sidecar_log}" 2>/dev/null | sed 's/^/    sidecar: /' || true

if [ "${rc}" -ne 0 ]; then
    echo "INPUT-SMOKE: FAIL (canary did not receive the canonical input sequence; rc=${rc})"
    exit 1
fi

# Focus acceptance: ownership is explicit and logged. Assert the
# sidecar recorded setting the guest's X input focus to the canary's xid -- the sidecar is the SOLE X
# focus authority (the ICD never touches focus). This promotes the prior display-only grep to a gate.
if ! grep -qE "focus -> xid=${XID} " "${sidecar_log}" 2>/dev/null; then
    echo "INPUT-SMOKE: FAIL (sidecar did not log focus ownership for xid=${XID})"
    exit 1
fi
echo "INPUT-SMOKE: focus ownership logged for xid=${XID} (sidecar = sole X focus authority)"

# Stale-input gate: no real input may be silently discarded. The canary received
# the full sequence above, so the sidecar must NOT have dropped any event for it as stale -- a
# "drop stale input xid=<canary>" line means the epoch reconciliation regressed.
if grep -qE "drop stale input xid=${XID} " "${sidecar_log}" 2>/dev/null; then
    echo "INPUT-SMOKE: FAIL (sidecar dropped input for xid=${XID} as stale -- epoch reconciliation"
    echo "             regressed):"
    grep -E "drop stale input xid=${XID} " "${sidecar_log}" | sed 's/^/    /'
    exit 1
fi
echo "INPUT-SMOKE: no stale-input drops for xid=${XID} (epoch reconciliation healthy)"

echo "============================================================"
echo "INPUT-SMOKE: PASS (sidecar drained the worker ring + injected focus/click/wheel/key/close;"
echo "             the input canary received the exact canonical sequence; focus ownership logged)"
echo "============================================================"
exit 0
