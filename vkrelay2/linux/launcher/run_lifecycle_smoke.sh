#!/usr/bin/env bash
# vkrelay2 boundary smoke: the show/hide LIFECYCLE over the wire (sidecar->worker). Brings
# up the private rootless-Xwayland stack + a (mock-backend) daemon + a session, starts the real
# sidecar (WM), then maps vkrelay2-lifecycle-canary: ONE normal toplevel that maps, UNMAPS itself, and
# REMAPS itself. Under the sidecar WM (SubstructureRedirect) the unmap is forwarded as
# set_visibility(Hidden) -- hidden-but-represented, NOT a teardown -- and the remap (a MapRequest of a
# still-tracked window) as set_visibility(Visible), a RESTORE.
#
# The PROOF is over the wire, two-fold:
#   1. the SIDECAR LOG shows the event mapping: register (at epoch E) + set_visibility state=1 (Hidden)
#      + set_visibility state=0 (Visible), all applied=1; and
#   2. after the cycle, the WORKER (DebugEnumWindows, via vkrelay2-geometry-query) reports the SAME
#      entry visible again at the SAME representation epoch E -- i.e. the hide kept the representation
#      (the bug fix: a hide is not an unregister) and the remap restored it without a rebuild.
#
# Mock backend: WSL has no GPU; the mock mirrors the visibility DECISION (host_visible == authored
# Visible) + the epoch (unchanged across hide/show), which is exactly what this proves (the real
# SW_HIDE/SW_SHOWNA + the reveal predicate are proven by the Windows integration_real_backend test).
# Skips cleanly (exit 0) when the private-display tools / built binaries are absent.
#
# Usage: bash run_lifecycle_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "LIFECYCLE-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-lifecycle-canary"
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

vkr_start_private_display || { echo "LIFECYCLE-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "LIFECYCLE-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "LIFECYCLE-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id lifecycle-smoke)" \
    || { echo "LIFECYCLE-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "LIFECYCLE-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "LIFECYCLE-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"

# Map the lifecycle canary; it maps -> unmaps -> remaps (synchronizing on the WM's StructureNotify),
# then prints its xid + done=1 and idles.
canary_log="${VKR_RUNTIME_DIR}/lifecycle_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 100); do
    XID="$(sed -n 's/^LIFECYCLE-CANARY: xid=\([0-9]\+\) .*done=1.*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "LIFECYCLE-SMOKE: FAIL (canary did not complete its map/unmap/remap)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "LIFECYCLE-SMOKE: canary xid=${XID} completed map/unmap/remap"

# (1) The sidecar event mapping: learn the register epoch E, then wait for the hide (state=1) + the
# restore (state=0) forwards, all applied=1.
EPOCH=""
for _ in $(seq 1 100); do
    EPOCH="$(sed -n "s/^.*register_toplevel xid=${XID} .* epoch=\([0-9]\+\) applied=1.*/\1/p" "${sidecar_log}" 2>/dev/null | head -1)"
    [ -n "${EPOCH}" ] && break
    sleep 0.1
done
[ -n "${EPOCH}" ] || { echo "LIFECYCLE-SMOKE: FAIL (sidecar did not register xid ${XID})"; grep -E "xid=${XID}" "${sidecar_log}" 2>/dev/null | sed 's/^/    /'; exit 1; }
hide_ok=""
show_ok=""
for _ in $(seq 1 100); do
    grep -qE "set_visibility xid=${XID} .* state=1 applied=1" "${sidecar_log}" 2>/dev/null && hide_ok=1
    grep -qE "set_visibility xid=${XID} .* state=0 applied=1" "${sidecar_log}" 2>/dev/null && show_ok=1
    [ -n "${hide_ok}" ] && [ -n "${show_ok}" ] && break
    sleep 0.1
done
[ -n "${hide_ok}" ] && [ -n "${show_ok}" ] || {
    echo "LIFECYCLE-SMOKE: FAIL (sidecar did not forward hide(state=1)+restore(state=0) for xid ${XID})"
    grep -E "set_visibility xid=${XID}" "${sidecar_log}" 2>/dev/null | sed 's/^/    /'
    exit 1
}
echo "LIFECYCLE-SMOKE: sidecar forwarded register(epoch=${EPOCH}) + hide + restore for xid ${XID}"

# (2) The worker registry: stop the sidecar (the worker serves one sidecar connection at a time) and
# prove the SAME entry is visible again at the SAME epoch -- the representation survived the hide/show.
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "LIFECYCLE-SMOKE: querying the worker for xid=${XID} (expect visible=1 @ epoch=${EPOCH})"
if ! "${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${XID}" --expect-x 60 --expect-y 80 --expect-visible 1 --expect-epoch "${EPOCH}" 2>&1 | sed 's/^/    /'; then
    echo "LIFECYCLE-SMOKE: FAIL (worker did not converge to visible@same-epoch after the hide/show)"
    exit 1
fi
echo "LIFECYCLE-SMOKE: PASS"
