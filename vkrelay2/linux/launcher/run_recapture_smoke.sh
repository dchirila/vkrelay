#!/usr/bin/env bash
# vkrelay2 boundary smoke: the sidecar's chrome RECAPTURE picks up a non-GL window's content that
# is painted a beat AFTER its first Expose. Same bring-up as run_chrome_smoke (private rootless-Xwayland
# + a mock-backend daemon + a session + the real sidecar), but the canary runs in DELAYED-COLOR mode:
# it maps BLACK, then ~600 ms later switches to the known color and repaints WITH NO Expose. So the late
# color can reach the worker ONLY via the sidecar's recapture poll, not an Expose-triggered ship. The
# PROOF is worker-visible over the wire: after recapture has had time to run, the sidecar is stopped and
# vkrelay2-chrome-query reads the worker's DIB via DebugChromeState -- PASS iff it sampled the COLOR
# (0xFF336699), not the initial black. With Expose-only capture the DIB stays black and
# this FAILS, so it is a real regression gate.
#
# Mock backend (WSL has no real GPU): this proves the recapture capture/transport/paint WIRING, not GPU
# pixels (mock == real for the paint path). Skips cleanly (exit 0) when the private-display tools / built
# binaries / libxcb-composite are absent.
#
# Usage: bash run_recapture_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir=""
_args=("$@")
_i=0
while [ ${_i} -lt ${#_args[@]} ]; do
    case "${_args[${_i}]}" in
        *) [ -z "${build_dir}" ] && build_dir="${_args[${_i}]}" ;;
    esac
    _i=$((_i + 1))
done
build_dir="${build_dir:-${script_dir}/../../build/linux-debug}"

skip() { echo "RECAPTURE-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-chrome-canary"
query="${build_dir}/vkrelay2-chrome-query"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}" "${query}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The canary's late fill is 0x336699 (RRGGBB); the worker's captured BGRA samples to this.
EXPECT_BGRA="0xFF336699"
DELAY_MS=600

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

vkr_start_private_display || { echo "RECAPTURE-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "RECAPTURE-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "RECAPTURE-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id recapture-smoke)" \
    || { echo "RECAPTURE-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "RECAPTURE-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "RECAPTURE-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
if ! grep -q "chrome capture enabled" "${sidecar_log}" 2>/dev/null; then
    skip "chrome capture not enabled (no libxcb-composite / no XComposite extension)"
fi

# Map the DELAYED-COLOR canary; learn its XID from its stdout.
canary_log="${VKR_RUNTIME_DIR}/canary.log"
DISPLAY="${DISPLAY}" "${canary}" --delay-color-ms "${DELAY_MS}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^CHROME-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "RECAPTURE-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "RECAPTURE-SMOKE: canary mapped BLACK, xid=${XID}; it will switch to ${EXPECT_BGRA} after ${DELAY_MS}ms with NO Expose"

# Wait for the canary to perform its no-Expose color switch (synchronization, not the proof).
colored=""
for _ in $(seq 1 100); do
    grep -q "CHROME-CANARY: colored" "${canary_log}" 2>/dev/null && { colored=1; break; }
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${colored}" ] || { echo "RECAPTURE-SMOKE: FAIL (canary never switched to the color)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }

# Give the sidecar's warm-up recapture time to notice the changed pixels and ship them. The first
# (black) paint shipped on the map Expose; the color must arrive via a LATER recapture ship.
sleep 1.5
paints="$(grep -c "paint_chrome xid=${XID} " "${sidecar_log}" 2>/dev/null || echo 0)"
echo "RECAPTURE-SMOKE: ${paints} paint_chrome ship(s) for xid=${XID} (1st=black on Expose, later=recapture color)"
grep -E "paint_chrome xid=${XID} " "${sidecar_log}" 2>/dev/null | tail -2 | sed 's/^/    /'
if [ "${paints}" -lt 2 ]; then
    echo "RECAPTURE-SMOKE: FAIL (expected >=2 ships: the Expose black + a recapture color)"
    exit 1
fi

# Hard gate: stop the capturing sidecar (the worker serves one sidecar connection), then read the
# worker's painted DIB over the wire. PASS iff it is the COLOR -- which was painted with NO Expose, so
# only recapture could have delivered it.
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
echo "RECAPTURE-SMOKE: querying the worker (DebugChromeState) for xid=${XID}; expecting ${EXPECT_BGRA}"
if ! "${query}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
        --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" --sample-x 5 --sample-y 5 \
        --expect-bgra "${EXPECT_BGRA}" 2>&1 | sed 's/^/    /'; then
    echo "RECAPTURE-SMOKE: FAIL (worker DIB is not the late color -> recapture did not ship the post-Expose paint)"
    exit 1
fi

echo "============================================================"
echo "RECAPTURE-SMOKE: PASS (a non-GL window painted ${EXPECT_BGRA} a beat after its first Expose with"
echo "                 NO further Expose; the sidecar recaptured it and the worker DIB holds the color)"
echo "============================================================"
exit 0
