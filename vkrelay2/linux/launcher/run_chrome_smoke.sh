#!/usr/bin/env bash
# vkrelay2 boundary smoke: REAL XComposite chrome capture, end to end. Brings up the private
# rootless-Xwayland stack + a (mock-backend) daemon + a session, starts the real sidecar (which claims
# the WM and enables chrome capture), then maps a pure-chrome canary (vkrelay2-chrome-canary, a solid
# known-color toplevel, NO Vulkan). The sidecar XComposite-captures the canary and ships it to the
# worker, which paints it into the placeholder DIB. The PROOF is worker-VISIBLE and over the wire (not a
# log): after the capture lands, the sidecar is stopped (the worker serves one sidecar connection at a
# time) and vkrelay2-chrome-query issues the DebugChromeState RPC for the canary's XID -- PASS iff the
# worker reports the placeholder shown and the sampled pixel equals the canary's color (0xFF336699).
#
# Mock backend: WSL has no real GPU, and this proves the chrome capture/transport/paint wiring, not GPU
# pixels. Skips cleanly (exit 0) when the private-display tools / built binaries / libxcb-composite are
# absent, so it is safe to run anywhere.
#
# Usage: bash run_chrome_smoke.sh [<build-dir>] [--save-png <dir>] [--shape-center]
#        [--shape-center-after-ms <ms>]
#   --save-png <dir>: after the worker-visible query gate, ALSO save a PNG of the captured chrome via
#   vkrelay2-capture and assert it is a valid PNG containing the canary color. The PNG is an
#   artifact; the DebugChromeState query stays the hard gate.   (default build-dir: ../../build/linux-debug)
#   --shape-center: apply a center-only XShape BOUNDING region and prove an outside pixel is defined
#   black while an inside pixel retains the canary color (the xeyes stale-corner regression).
#   --shape-center-after-ms <ms>: map rectangular, then change only the shape after the delay with
#   no Expose; proves the steady recapture poll notices and ships a shape-only change.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
# Parse a COPY of the args (leave "$@" intact for the namespace re-exec, which re-runs this script).
build_dir=""
save_png=""
shape_center=0
shape_center_after_ms=""
_args=("$@")
_i=0
while [ ${_i} -lt ${#_args[@]} ]; do
    case "${_args[${_i}]}" in
        --save-png) _i=$((_i + 1)); save_png="${_args[${_i}]:-}" ;;
        --shape-center) shape_center=1 ;;
        --shape-center-after-ms)
            _i=$((_i + 1)); shape_center_after_ms="${_args[${_i}]:-}"; shape_center=1 ;;
        *) [ -z "${build_dir}" ] && build_dir="${_args[${_i}]}" ;;
    esac
    _i=$((_i + 1))
done
build_dir="${build_dir:-${script_dir}/../../build/linux-debug}"

skip() { echo "CHROME-SMOKE: SKIP ($1)"; exit 0; }

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

# The canary fills its window with 0x336699 (RRGGBB); the worker's captured BGRA samples to this.
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

vkr_reexec_in_private_x11_namespace "$@" || exit $?

vkr_start_private_display || { echo "CHROME-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "CHROME-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "CHROME-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id chrome-smoke)" \
    || { echo "CHROME-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "CHROME-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "CHROME-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
# Chrome capture must be compiled in (libxcb-composite) AND XComposite present, else there is nothing
# to prove -- skip cleanly.
if ! grep -q "chrome capture enabled" "${sidecar_log}" 2>/dev/null; then
    skip "chrome capture not enabled (no libxcb-composite / no XComposite extension)"
fi
if [ "${shape_center}" -eq 1 ] && ! grep -q "shape masking enabled" "${sidecar_log}" 2>/dev/null; then
    skip "shape masking not enabled (no libxcb-shape / no SHAPE extension)"
fi

# Map the pure-chrome canary; learn its XID from its stdout.
canary_log="${VKR_RUNTIME_DIR}/canary.log"
canary_args=()
if [ -n "${shape_center_after_ms}" ]; then
    canary_args+=(--shape-center-after-ms "${shape_center_after_ms}")
elif [ "${shape_center}" -eq 1 ]; then
    canary_args+=(--shape-center)
fi
DISPLAY="${DISPLAY}" "${canary}" "${canary_args[@]}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^CHROME-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "CHROME-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "CHROME-SMOKE: canary mapped, xid=${XID}; waiting for the sidecar to capture + paint it"

if [ -n "${shape_center_after_ms}" ]; then
    shaped=""
    for _ in $(seq 1 100); do
        grep -q "CHROME-CANARY: shaped" "${canary_log}" 2>/dev/null && { shaped=1; break; }
        kill -0 "${CANARY_PID}" 2>/dev/null || break
        sleep 0.1
    done
    [ -n "${shaped}" ] || { echo "CHROME-SMOKE: FAIL (canary never changed shape)"; exit 1; }
    # No ShapeNotify is selected by the current sidecar. Give the existing recapture poll enough
    # time to observe, hash, and ship this no-Expose shape-only change.
    sleep 1
    if grep -q "CHROME-CANARY: post-shape Expose" "${canary_log}" 2>/dev/null; then
        echo "CHROME-SMOKE: FAIL (shape-only canary unexpectedly received an Expose)"
        exit 1
    fi
    echo "CHROME-SMOKE: shape-only transition produced no Expose; recapture poll must refresh it"
fi

# Wait (synchronization, NOT the proof) for the sidecar to ship a successful paint of the canary.
paint_ok=""
for _ in $(seq 1 100); do
    grep -q "paint_chrome xid=${XID} .* applied=1 shown=1" "${sidecar_log}" 2>/dev/null && { paint_ok=1; break; }
    sleep 0.1
done
[ -n "${paint_ok}" ] || {
    echo "CHROME-SMOKE: FAIL (no successful paint_chrome for xid=${XID})"
    grep -E "paint_chrome|chrome capture" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
grep -E "paint_chrome xid=${XID} " "${sidecar_log}" 2>/dev/null | tail -1 | sed 's/^/    /'

# The worker serves one sidecar connection at a time, so stop the capturing sidecar to free the plane,
# then prove the worker's painted DIB over the wire (the worker-visible structured query).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
echo "CHROME-SMOKE: querying the worker (DebugChromeState) for xid=${XID}"
if [ "${shape_center}" -eq 1 ]; then
    if ! "${query}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
            --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" \
            --sample-x 5 --sample-y 5 --expect-bgra 0xFF000000 2>&1 | sed 's/^/    /'; then
        echo "CHROME-SMOKE: FAIL (out-of-shape worker pixel was not defined black)"
        exit 1
    fi
    if ! "${query}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
            --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" \
            --sample-x 100 --sample-y 75 --expect-bgra "${EXPECT_BGRA}" 2>&1 | sed 's/^/    /'; then
        echo "CHROME-SMOKE: FAIL (in-shape worker pixel lost the canary color)"
        exit 1
    fi
else
    if ! "${query}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
            --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" --sample-x 5 --sample-y 5 \
            --expect-bgra "${EXPECT_BGRA}" 2>&1 | sed 's/^/    /'; then
        echo "CHROME-SMOKE: FAIL (worker did not paint the captured chrome)"
        exit 1
    fi
fi

# Optional artifact: save a PNG of the captured chrome from the worker's own DIB (a sequential
# sidecar-plane connection after the query). The PNG is evidence; the query above is the hard gate.
if [ -n "${save_png}" ]; then
    capture="${build_dir}/vkrelay2-capture"
    # --save-png is an EXPLICIT request to prove the PNG path, so a missing tool is a FAILURE, not a
    # skip.
    if [ ! -x "${capture}" ]; then
        echo "CHROME-SMOKE: FAIL (--save-png requested but vkrelay2-capture is missing: ${capture})"
        exit 1
    else
        mkdir -p "${save_png}"
        echo "CHROME-SMOKE: saving PNG via vkrelay2-capture (xid=${XID}, layer=chrome)"
        if ! "${capture}" --connect "${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}" \
                --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" --xid "${XID}" --layer chrome \
                --out "${save_png}/chrome_${XID}" --expect-bgra "${EXPECT_BGRA}" 2>&1 | sed 's/^/    /'; then
            echo "CHROME-SMOKE: FAIL (vkrelay2-capture did not produce a valid PNG with the canary color)"
            exit 1
        fi
        echo "CHROME-SMOKE: PNG saved -> ${save_png}/chrome_${XID}.png (+ .json)"
    fi
fi

echo "============================================================"
echo "CHROME-SMOKE: PASS (sidecar XComposite-captured the canary; the worker painted it;"
if [ "${shape_center}" -eq 1 ]; then
    echo "              XShape outside=0xFF000000 and inside=${EXPECT_BGRA})"
else
    echo "              DebugChromeState sampled the canary color ${EXPECT_BGRA})"
fi
echo "============================================================"
exit 0
