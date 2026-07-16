#!/usr/bin/env bash
# vkrelay2 boundary smoke: REAL override-redirect popup representation, end to end. Brings
# up the private rootless-Xwayland stack + a (mock-backend) daemon + a session, starts the real sidecar
# (WM + capture), then maps vkrelay2-popup-canary: a normal toplevel (the owner) + an override-redirect
# child popup advertising _NET_WM_WINDOW_TYPE_MENU + WM_TRANSIENT_FOR = the toplevel. The sidecar detects
# the self-mapped popup via MapNotify, classifies it, resolves its owner, and asks the worker to give it
# a placeholder-class host OWNED by + above the owner.
#
# The PROOF is worker-VISIBLE and over the wire (not a log): after the popup registers, the sidecar is
# stopped (the worker serves one sidecar connection at a time) and:
#   - vkrelay2-popup-query (DebugEnumWindows) asserts the popup is represented with is_popup=true +
#     owner_xid == the toplevel, and the owner is a registered non-popup toplevel -- the headline
#     owner/z-order linkage. This gate runs regardless of XComposite.
#   - if chrome capture is enabled (libxcb-composite + XComposite), vkrelay2-chrome-query also asserts
#     the popup's captured chrome reached the worker DIB (sampled pixel == the popup color 0xFF9933CC).
#
# Mock backend: WSL has no real GPU, and this proves the popup detection/owner/representation wiring, not
# GPU pixels. Skips cleanly (exit 0) when the private-display tools / built binaries are absent.
#
# Usage: bash run_popup_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "POPUP-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-popup-canary"
query="${build_dir}/vkrelay2-popup-query"
chrome_query="${build_dir}/vkrelay2-chrome-query"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}" "${query}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The popup fills its window with 0x9933CC (RRGGBB); the worker's captured BGRA samples to this.
EXPECT_BGRA="0xFF9933CC"

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

vkr_start_private_display || { echo "POPUP-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "POPUP-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "POPUP-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id popup-smoke)" \
    || { echo "POPUP-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "POPUP-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "POPUP-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
chrome_enabled=""
grep -q "chrome capture enabled" "${sidecar_log}" 2>/dev/null && chrome_enabled=1

# Map the popup canary (owner toplevel + override-redirect popup); learn both XIDs from its stdout.
# --delay-color-ms: the popup maps BLACK and paints its content color AFTER map with no Expose (a real
# Qt menu), so ONLY chrome recapture can carry it to the worker. On
# code that does not recapture popups the worker keeps the black first frame (the chrome sub-gate below
# then fails); with popup recapture the content reaches the worker (it passes).
POPUP_DELAY_MS=400
canary_log="${VKR_RUNTIME_DIR}/canary.log"
DISPLAY="${DISPLAY}" "${canary}" --delay-color-ms "${POPUP_DELAY_MS}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
TOPLEVEL="" POPUP=""
for _ in $(seq 1 50); do
    TOPLEVEL="$(sed -n 's/^POPUP-CANARY: toplevel=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    POPUP="$(sed -n 's/^POPUP-CANARY: .*popup=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${TOPLEVEL}" ] && [ -n "${POPUP}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${TOPLEVEL}" ] && [ -n "${POPUP}" ] \
    || { echo "POPUP-SMOKE: FAIL (canary did not report its xids)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "POPUP-SMOKE: canary mapped, toplevel=${TOPLEVEL} popup=${POPUP}; waiting for the sidecar to register the popup"

# Wait (synchronization, NOT the proof) for the sidecar to register the popup with its owner.
reg_ok=""
for _ in $(seq 1 100); do
    grep -qE "register_toplevel xid=${POPUP} .* popup=1 owner=${TOPLEVEL}" "${sidecar_log}" 2>/dev/null && { reg_ok=1; break; }
    sleep 0.1
done
[ -n "${reg_ok}" ] || {
    echo "POPUP-SMOKE: FAIL (sidecar did not register popup ${POPUP} owned by ${TOPLEVEL})"
    grep -E "register_toplevel|popup drop" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
grep -E "register_toplevel xid=${POPUP} " "${sidecar_log}" 2>/dev/null | tail -1 | sed 's/^/    /'

# If chrome capture is on, drive the paint-after-map recapture gate before sampling the popup pixel.
if [ -n "${chrome_enabled}" ]; then
    # 1) The popup's first COMMITTED + SHOWN paint. With the black-flash fix the premature all-black
    #    map frame is SUPPRESSED (never shipped, so the popup stays hidden instead of flashing
    #    black), so this first shown paint is already the CONTENT frame -- it appears once the
    #    canary's delayed paint lands and a recapture carries it. Bounded, non-fatal.
    for _ in $(seq 1 100); do
        grep -q "paint_chrome xid=${POPUP} .* applied=1 shown=1" "${sidecar_log}" 2>/dev/null && break
        sleep 0.1
    done
    # 2) Wait for the canary's paint-AFTER-map (content color now in the redirected pixmap, no Expose).
    marker=""
    for _ in $(seq 1 100); do
        grep -q "POPUP-CANARY: delayed-paint applied" "${canary_log}" 2>/dev/null && { marker=1; break; }
        kill -0 "${CANARY_PID}" 2>/dev/null || break
        sleep 0.1
    done
    [ -n "${marker}" ] || { echo "POPUP-SMOKE: FAIL (canary never applied its delayed popup paint)"; exit 1; }
    # 3) do NOT stop the sidecar until a chrome RECAPTURE has run AFTER the delayed
    #    paint -- otherwise we would sample the known-black FIRST frame even when the fix is correct.
    #    Wait for one more committed popup paint past the count at the marker, then settle so the
    #    recaptured CONTENT frame is the worker's latest. On code that does not recapture popups the
    #    count never advances (bounded wait, then the chrome sub-gate below fails on the black frame).
    base="$(grep -cE "paint_chrome xid=${POPUP} .* applied=1" "${sidecar_log}" 2>/dev/null)"
    for _ in $(seq 1 100); do
        cur="$(grep -cE "paint_chrome xid=${POPUP} .* applied=1" "${sidecar_log}" 2>/dev/null)"
        [ "${cur:-0}" -gt "${base:-0}" ] && break
        sleep 0.1
    done
    sleep 0.3
fi

# The worker serves one sidecar connection at a time, so stop the sidecar to free the plane, then prove
# the popup's owner/z-order linkage over the wire (the worker-visible structured query).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "POPUP-SMOKE: querying the worker (DebugEnumWindows) for popup=${POPUP} owner=${TOPLEVEL}"
# Capture-then-check: a `| sed` pipe would mask the query's exit code (no pipefail), so the sub-gate
# would never actually gate. Grab the output + status, print indented, THEN honor the status.
if query_out="$("${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --popup-xid "${POPUP}" --expect-owner "${TOPLEVEL}" 2>&1)"; then query_rc=0; else query_rc=$?; fi
printf '%s\n' "${query_out}" | sed 's/^/    /'
if [ "${query_rc}" -ne 0 ]; then
    echo "POPUP-SMOKE: FAIL (worker did not represent the popup owned by the toplevel)"
    exit 1
fi

# Optional pixel sub-gate: the popup's captured chrome reached the worker DIB (needs XComposite).
if [ -n "${chrome_enabled}" ] && [ -x "${chrome_query}" ]; then
    echo "POPUP-SMOKE: querying the worker (DebugChromeState) for popup=${POPUP} pixel"
    if chrome_out="$("${chrome_query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
            --xid "${POPUP}" --sample-x 5 --sample-y 5 --expect-bgra "${EXPECT_BGRA}" 2>&1)"; then
        chrome_rc=0
    else
        chrome_rc=$?
    fi
    printf '%s\n' "${chrome_out}" | sed 's/^/    /'
    if [ "${chrome_rc}" -ne 0 ]; then
        echo "POPUP-SMOKE: FAIL (popup chrome did not reach the worker DIB as ${EXPECT_BGRA})"
        exit 1
    fi
else
    echo "POPUP-SMOKE: chrome pixel sub-gate skipped (no XComposite); owner/z-order linkage proven"
fi

echo "============================================================"
echo "POPUP-SMOKE: PASS (sidecar detected the override-redirect popup, resolved its owner,"
echo "             and the worker represents it owned by the toplevel)"
echo "============================================================"
exit 0
