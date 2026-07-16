#!/usr/bin/env bash
# vkrelay2 guest-display-geometry boundary smoke: the guest X root must be SIZEABLE to the host's
# addressable space, and pointer injection must reach coordinates BEYOND the old default root. This is
# the OpenSCAD unclickable-region bug shape, end to end: the old weston-headless default root is
# 1024x640 while the app window is host-monitor-sized, and the injection warp
# (xcb_test_fake_input(MOTION, absolute, root, ...)) is CLAMPED by the X server to the root -- so
# every client point beyond the root edge was unreachable (menus/editor dead past a line).
#
# By default the smoke sets VKRELAY2_GUEST_ROOT (the explicit legacy test override). With
# VKRELAY2_BIGROOT_EXTERNAL_DAEMON=1 it instead queries the configured production daemon before
# Weston starts and consumes the advertised snapshot/virtual bounds. The default lane maps a
# canary LARGER than the OLD default root; the live lane uses a normal anchor plus a root listener.
# The sidecar then seeds ONE click at guest-root (1200,800), which
# lies beyond 1024x640, through the production worker-ring -> PollInput -> XTest path. PASS iff the
# canary reports (a) the root size == the requested override and (b) the click arrived at EXACTLY
# (1200,800). On pre-GD code the override is ignored (root stays 1024x640) and the warp clamps, so
# this FAILS -- the RED gate.
#
# The default local mock backend proves display sizing + the un-clamped injection path. External
# mode is the live acceptance lane and exercises the Windows real worker + pinned snapshot.
# Skips cleanly (exit 0) when the private-display tools / built binaries are absent.
#
# Usage: bash run_bigroot_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
# Live example: VKRELAY2_BIGROOT_EXTERNAL_DAEMON=1 VKRELAY2_BIGROOT_CLICK=4480,858 \
#   VKRELAY2_DAEMON_HOST=<windows-host> VKRELAY2_DAEMON_PORT=<port> bash run_bigroot_smoke.sh
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "BIGROOT-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-bigroot-canary"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The default lane retains the original 1600x1000 / 1200,800 RED gate. The external lane gets
# the root from --query-host-display and permits either live-machine acceptance point.
GUEST_ROOT_W=1600
GUEST_ROOT_H=1000
CLICK_SPEC="${VKRELAY2_BIGROOT_CLICK:-1200,800}"
CLICK_X="${CLICK_SPEC%%,*}"
CLICK_Y="${CLICK_SPEC##*,}"
if [[ ! "${CLICK_X}" =~ ^[0-9]+$ ]] || [[ ! "${CLICK_Y}" =~ ^[0-9]+$ ]]; then
    echo "BIGROOT-SMOKE: FAIL (VKRELAY2_BIGROOT_CLICK expects non-negative X,Y)"
    exit 1
fi
if [ "${VKRELAY2_BIGROOT_EXTERNAL_DAEMON:-0}" != "1" ]; then
    unset VKRELAY2_DISPLAY_SNAPSHOT_ID VKRELAY2_HOST_VIRTUAL_W VKRELAY2_HOST_VIRTUAL_H
    export VKRELAY2_GUEST_ROOT="${GUEST_ROOT_W}x${GUEST_ROOT_H}"
fi

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

if [ "${VKRELAY2_BIGROOT_EXTERNAL_DAEMON:-0}" = "1" ]; then
    unset VKRELAY2_DISPLAY_SNAPSHOT_ID VKRELAY2_HOST_VIRTUAL_W VKRELAY2_HOST_VIRTUAL_H
    display_env="$("${launch}" --query-host-display)" \
        || { echo "BIGROOT-SMOKE: FAIL (could not query external daemon topology)"; exit 1; }
    while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${display_env}"
    [ -n "${VKRELAY2_DISPLAY_SNAPSHOT_ID:-}" ] \
        || { echo "BIGROOT-SMOKE: FAIL (external daemon advertised no display snapshot)"; exit 1; }
    [ -n "${VKRELAY2_HOST_VIRTUAL_W:-}" ] && [ -n "${VKRELAY2_HOST_VIRTUAL_H:-}" ] \
        || { echo "BIGROOT-SMOKE: FAIL (external daemon advertised no virtual bounds)"; exit 1; }
    GUEST_ROOT_W="${VKRELAY2_HOST_VIRTUAL_W}"
    GUEST_ROOT_H="${VKRELAY2_HOST_VIRTUAL_H}"
    unset VKRELAY2_GUEST_ROOT
    echo "BIGROOT-SMOKE: queried snapshot ${VKRELAY2_DISPLAY_SNAPSHOT_ID} virtual ${GUEST_ROOT_W}x${GUEST_ROOT_H}"
fi

if [ "${CLICK_X}" -ge "${GUEST_ROOT_W}" ] || [ "${CLICK_Y}" -ge "${GUEST_ROOT_H}" ]; then
    echo "BIGROOT-SMOKE: FAIL (guest click ${CLICK_X},${CLICK_Y} lies outside ${GUEST_ROOT_W}x${GUEST_ROOT_H})"
    exit 1
fi
# The legacy lane keeps its intentionally oversized 1400x900 window. The external lane uses a normal
# 600x600 window centered around the requested guest point: that isolates virtual-origin mapping
# from Win32's legitimate constraints on a window wider than the 1200px laptop monitor.
if [ "${VKRELAY2_BIGROOT_EXTERNAL_DAEMON:-0}" = "1" ]; then
    CANARY_W=600
    CANARY_H=600
    CANARY_X=$((CLICK_X - CANARY_W / 2))
    CANARY_Y=$((CLICK_Y - CANARY_H / 2))
    [ "${CANARY_X}" -lt 0 ] && CANARY_X=0
    [ "${CANARY_Y}" -lt 0 ] && CANARY_Y=0
else
    CANARY_W=1400
    CANARY_H=900
    CANARY_X=0
    CANARY_Y=0
fi
if [ $((CANARY_X + CANARY_W)) -gt "${GUEST_ROOT_W}" ]; then
    CANARY_X=$((GUEST_ROOT_W - CANARY_W))
fi
if [ $((CANARY_Y + CANARY_H)) -gt "${GUEST_ROOT_H}" ]; then
    CANARY_Y=$((GUEST_ROOT_H - CANARY_H))
fi
vkr_start_private_display || { echo "BIGROOT-SMOKE: FAIL (private display setup)"; exit 1; }

if [ "${VKRELAY2_BIGROOT_EXTERNAL_DAEMON:-0}" != "1" ]; then
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
    [ -n "${ok}" ] || { echo "BIGROOT-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
    export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
    export VKRELAY2_DAEMON_HOST=127.0.0.1
    echo "BIGROOT-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"
fi

session_env="$("${launch}" --open-session --app-id bigroot-smoke)" \
    || { echo "BIGROOT-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "BIGROOT-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

# The sidecar seeds ONE click through the production worker-ring -> XTest injection path.
vkr_start_sidecar_and_wait_ready "${sidecar}" --debug-inject-click "${CLICK_X},${CLICK_Y}" || {
    echo "BIGROOT-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

# Map the bigroot canary; learn its xid + REPORTED ROOT SIZE from its stdout.
canary_log="${VKR_RUNTIME_DIR}/bigroot_canary.log"
DISPLAY="${DISPLAY}" "${canary}" "${CANARY_W}" "${CANARY_H}" "${CANARY_X}" "${CANARY_Y}" \
    >"${canary_log}" 2>&1 &
CANARY_PID=$!
ROOT=""
for _ in $(seq 1 50); do
    ROOT="$(sed -n 's/^BIGROOT-CANARY: xid=[0-9]* root=\([0-9]*x[0-9]*\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${ROOT}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${ROOT}" ] || { echo "BIGROOT-SMOKE: FAIL (canary did not report)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "BIGROOT-SMOKE: guest root=${ROOT} (requested ${GUEST_ROOT_W}x${GUEST_ROOT_H})"

# Gate 1: the private display honored the requested root size (the display-sizing plumbing).
if [ "${ROOT}" != "${GUEST_ROOT_W}x${GUEST_ROOT_H}" ]; then
    echo "BIGROOT-SMOKE: FAIL (guest root is ${ROOT}, not the requested ${GUEST_ROOT_W}x${GUEST_ROOT_H} -- the"
    echo "               display sizing is not plumbed; clicks beyond the default root stay unreachable)"
    exit 1
fi

# Gate 2: the injected click arrived at the exact guest-root coordinate. The event's client
# coordinate is retained in the diagnostic; when the target is empty root space it equals root.
CLICK=""
ROOT_CLICK=""
for _ in $(seq 1 150); do
    CLICK="$(sed -n 's/^BIGROOT-CANARY: click_at \(-\?[0-9]*,-\?[0-9]*\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    ROOT_CLICK="$(sed -n 's/^BIGROOT-CANARY: click_at .* root_at \(-\?[0-9]*,-\?[0-9]*\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${CLICK}" ] && [ -n "${ROOT_CLICK}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${CLICK}" ] || {
    echo "BIGROOT-SMOKE: FAIL (canary never received the injected click)"
    grep -E "debug-inject-click|register_toplevel" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}
echo "BIGROOT-SMOKE: click arrived client=${CLICK} guest=${ROOT_CLICK} (aimed guest ${CLICK_X},${CLICK_Y})"
if [ "${ROOT_CLICK}" != "${CLICK_X},${CLICK_Y}" ]; then
    echo "BIGROOT-SMOKE: FAIL (click landed client=${CLICK} guest=${ROOT_CLICK}, not"
    echo "               guest=${CLICK_X},${CLICK_Y} -- the pointer"
    echo "               warp was clamped by an undersized guest root: the unclickable-region bug)"
    exit 1
fi

echo "============================================================"
echo "BIGROOT-SMOKE: PASS (guest root sized to ${ROOT}; a click beyond the old 1024x640 default root"
echo "              landed at exactly guest ${ROOT_CLICK} -- no more unreachable window regions)"
echo "============================================================"
exit 0
