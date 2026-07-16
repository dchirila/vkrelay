#!/usr/bin/env bash
# vkrelay2 placement-clamp boundary smoke: the on-screen CLIENT-ORIGIN clamp, end to end. Same
# bring-up as run_placement_smoke (private rootless-Xwayland + a mock-backend daemon + a session + the
# real sidecar/WM), then maps vkrelay2-clamp-canary: an EXPLICITLY-positioned toplevel mapped off the
# top-left, which then self-ConfigureRequests off the left. The canary reads back its OWN X-root
# geometry (the authored truth -- the invariant lives where X geometry is authored,
# not the worker's separately-clamped host), so this proves the sidecar clamps the guest client origin
# on-screen on BOTH the map (maybe_center_toplevel) and the ConfigureRequest paths.
#
# The X-side readback is the primary proof; the smoke ALSO cross-checks the ConfigureRequest path
# worker-visible over the wire (vkrelay2-geometry-query): a non-initial off-left apply is NOT clamped
# by the worker, so without the clamp the host actual lands off-screen too. The sidecar would honor
# the off-screen requests verbatim and this FAILS; with the clamp both readbacks land at the expected
# targets (0,0 for the map; 0,100 for the off-left ConfigureRequest -- per-axis, y preserved).
#
# Mock backend (WSL has no real GPU): proves the sidecar clamp decision + transport + placement (mock
# == real for the authored origin). Skips cleanly (exit 0) when tools / built binaries are absent.
#
# Usage: bash run_clamp_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "CLAMP-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-clamp-canary"
query="${build_dir}/vkrelay2-geometry-query"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}" "${query}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The clamp targets: an off-top map (-100,-100) clamps to (0,0); an off-left ConfigureRequest
# (-50,100) clamps to (0,100) -- per-axis, so y is preserved.
EXPECT_MAP_X=0 EXPECT_MAP_Y=0
EXPECT_CFG_X=0 EXPECT_CFG_Y=100

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

vkr_start_private_display || { echo "CLAMP-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "CLAMP-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "CLAMP-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id clamp-smoke)" \
    || { echo "CLAMP-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "CLAMP-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "CLAMP-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

# Map the clamp canary; learn its xid from its stdout.
canary_log="${VKR_RUNTIME_DIR}/clamp_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^CLAMP-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "CLAMP-SMOKE: FAIL (canary did not report its xid)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "CLAMP-SMOKE: canary mapped, xid=${XID}"

# Wait for the canary's X-side readbacks (the ground truth of where the guest window landed).
MAPPED="" CONFIGURED=""
for _ in $(seq 1 100); do
    [ -z "${MAPPED}" ] && MAPPED="$(sed -n 's/^CLAMP-CANARY: mapped_at \(-\?[0-9]*\),\(-\?[0-9]*\).*/\1 \2/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -z "${CONFIGURED}" ] && CONFIGURED="$(sed -n 's/^CLAMP-CANARY: configured_at \(-\?[0-9]*\),\(-\?[0-9]*\).*/\1 \2/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${MAPPED}" ] && [ -n "${CONFIGURED}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${MAPPED}" ] && [ -n "${CONFIGURED}" ] || {
    echo "CLAMP-SMOKE: FAIL (canary did not report both readbacks)"
    sed 's/^/    /' "${canary_log}" 2>/dev/null
    exit 1
}
MX="${MAPPED%% *}"; MY="${MAPPED##* }"
CX="${CONFIGURED%% *}"; CY="${CONFIGURED##* }"
echo "CLAMP-SMOKE: X-side readback mapped_at=${MX},${MY} configured_at=${CX},${CY}"

fail=""
if [ "${MX}" -ne "${EXPECT_MAP_X}" ] || [ "${MY}" -ne "${EXPECT_MAP_Y}" ]; then
    echo "CLAMP-SMOKE: FAIL (map landed at ${MX},${MY}, not the clamped ${EXPECT_MAP_X},${EXPECT_MAP_Y} -- menu bar off-screen)"
    fail=1
fi
if [ "${CX}" -ne "${EXPECT_CFG_X}" ] || [ "${CY}" -ne "${EXPECT_CFG_Y}" ]; then
    echo "CLAMP-SMOKE: FAIL (ConfigureRequest landed at ${CX},${CY}, not the clamped ${EXPECT_CFG_X},${EXPECT_CFG_Y})"
    fail=1
fi
[ -z "${fail}" ] || exit 1

# Worker-visible cross-check of the ConfigureRequest path: a non-initial off-left apply is NOT clamped
# by the worker, so pre-fix the host actual lands off-screen too. Stop the sidecar (single connection)
# then assert the worker's actual host client origin converged to the clamped (0,100).
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "CLAMP-SMOKE: querying the worker (DebugEnumWindows) for xid=${XID} -> ${EXPECT_CFG_X},${EXPECT_CFG_Y}"
if query_out="$("${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${XID}" --expect-x "${EXPECT_CFG_X}" --expect-y "${EXPECT_CFG_Y}" 2>&1)"; then query_rc=0; else query_rc=$?; fi
printf '%s\n' "${query_out}" | sed 's/^/    /'
if [ "${query_rc}" -ne 0 ]; then
    echo "CLAMP-SMOKE: FAIL (worker host did not converge to the clamped ConfigureRequest origin)"
    exit 1
fi

echo "============================================================"
echo "CLAMP-SMOKE: PASS (off-screen app/sidecar-authored geometry was clamped on-screen on the X side"
echo "             -- map ${MX},${MY} + ConfigureRequest ${CX},${CY} -- menu bar stays reachable)"
echo "============================================================"
exit 0
