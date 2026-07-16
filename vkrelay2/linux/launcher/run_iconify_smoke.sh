#!/usr/bin/env bash
# vkrelay2 boundary smoke: a Win32-USER MINIMIZE then RESTORE over the wire (the
# REVERSE direction, worker->sidecar) + the pending-iconify echo guard.
# Brings up the private rootless-Xwayland stack + a (mock-backend) daemon + a session, starts the real
# sidecar with --debug-feedback-move X,Y --debug-feedback-minimize --debug-feedback-restore (which
# seeds, for the first toplevel, a Win32-user-origin GeometryRequest MOVE then {Minimize}, then -- once
# the iconify-unmap has been consumed -- a {Restore}; standing in for a caption-box / taskbar
# minimize+restore, which cannot originate on WSL), then maps vkrelay2-feedback-canary.
#
# The PROOF is over the wire, three-fold:
#   1. the SIDECAR authors the iconify: it sets WM_STATE=Iconic + unmaps the guest, tells the worker
#      Iconic (set_visibility state=2), and the guest's self-induced UnmapNotify is CONSUMED by the
#      pending-iconify guard -- the sidecar log shows "user_iconify" + "unmap consumed by
#      pending-iconify" and NO state=1 (Hidden) downgrade for the xid (the central risk);
#   2. the SIDECAR then authors the RESTORE over the wire: apply_user_restore maps the guest + tells
#      the worker Visible (set_visibility state=0) -- the sidecar log shows "user_restore"; and
#   3. the WORKER (DebugEnumWindows, via vkrelay2-geometry-query) converged to VISIBLE + NOT iconic
#      (host_visible=1 host_iconic=0) at the moved position -- the full minimize->restore loop landed.
# (The worker-visible ICONIC apply -- IsIconic && IsWindowVisible, taskbar-restorable, NOT SW_HIDE --
# is proven by the Windows integration_real_backend test_real_user_minimize_restore.)
#
# Mock backend: WSL has no GPU; the mock mirrors the visibility DECISION (mock == real for the
# lifecycle decision), which is exactly what this proves (the real SW_SHOWMINNOACTIVE/SW_SHOWNA +
# IsIconic are proven by the Windows integration_real_backend test). Skips cleanly (exit 0) when the
# private-display tools / built binaries are absent.
#
# Usage: bash run_iconify_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "ICONIFY-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-feedback-canary"
query="${build_dir}/vkrelay2-geometry-query"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}" "${query}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# The move target the sidecar seeds before the minimize (so the worker has a known position to query
# alongside the Iconic state); the canary starts at 50,60.
TARGET_X=260
TARGET_Y=180

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

vkr_start_private_display || { echo "ICONIFY-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "ICONIFY-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "ICONIFY-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id iconify-smoke)" \
    || { echo "ICONIFY-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "ICONIFY-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

# The sidecar seeds a Win32-user-origin move (to TARGET), a Minimize, then a Restore (the latter once
# the iconify-unmap has been consumed) for the first toplevel.
vkr_start_sidecar_and_wait_ready "${sidecar}" \
    --debug-feedback-move "${TARGET_X},${TARGET_Y}" --debug-feedback-minimize \
    --debug-feedback-restore || {
    echo "ICONIFY-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"

canary_log="${VKR_RUNTIME_DIR}/iconify_canary.log"
DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
CANARY_PID=$!
XID=""
for _ in $(seq 1 50); do
    XID="$(sed -n 's/^FEEDBACK-CANARY: xid=\([0-9]\+\) .*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
    [ -n "${XID}" ] && break
    kill -0 "${CANARY_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${XID}" ] || { echo "ICONIFY-SMOKE: FAIL (canary did not map)"; sed 's/^/    /' "${canary_log}" 2>/dev/null; exit 1; }
echo "ICONIFY-SMOKE: canary xid=${XID} mapped; expecting move->${TARGET_X},${TARGET_Y}, iconify, then restore"

# (1) The sidecar authored Iconic AND the pending-iconify guard CONSUMED the guest's self-induced
# UnmapNotify (no Hidden downgrade). Wait for set_visibility state=2 (Iconic) applied=1 + the consume.
icon_ok=""
consume_ok=""
for _ in $(seq 1 100); do
    grep -qE "set_visibility xid=${XID} .* state=2 applied=1" "${sidecar_log}" 2>/dev/null && icon_ok=1
    grep -qE "unmap consumed by pending-iconify xid=${XID}" "${sidecar_log}" 2>/dev/null && consume_ok=1
    [ -n "${icon_ok}" ] && [ -n "${consume_ok}" ] && break
    sleep 0.1
done
[ -n "${icon_ok}" ] && [ -n "${consume_ok}" ] || {
    echo "ICONIFY-SMOKE: FAIL (sidecar did not author Iconic + consume the iconify-unmap for xid ${XID})"
    grep -E "xid=${XID}" "${sidecar_log}" 2>/dev/null | sed 's/^/    /'
    exit 1
}
echo "ICONIFY-SMOKE: sidecar authored Iconic + consumed the iconify-unmap for xid ${XID}"

# (2) The sidecar then authors the RESTORE over the wire (apply_user_restore): wait for "user_restore"
# + set_visibility state=0 (Visible) applied=1 for the xid.
restore_ok=""
visible_ok=""
for _ in $(seq 1 100); do
    grep -qE "user_restore xid=${XID} " "${sidecar_log}" 2>/dev/null && restore_ok=1
    grep -qE "set_visibility xid=${XID} .* state=0 applied=1" "${sidecar_log}" 2>/dev/null && visible_ok=1
    [ -n "${restore_ok}" ] && [ -n "${visible_ok}" ] && break
    sleep 0.1
done
[ -n "${restore_ok}" ] && [ -n "${visible_ok}" ] || {
    echo "ICONIFY-SMOKE: FAIL (sidecar did not author the restore (Visible) for xid ${XID})"
    grep -E "xid=${XID}" "${sidecar_log}" 2>/dev/null | sed 's/^/    /'
    exit 1
}
# The guard must NOT have leaked a Hidden (state=1) downgrade for this xid at ANY point (the iconify
# stayed Iconic; the restore went straight to Visible).
if grep -qE "set_visibility xid=${XID} .* state=1 applied=1" "${sidecar_log}" 2>/dev/null; then
    echo "ICONIFY-SMOKE: FAIL (a Hidden downgrade leaked through the pending-iconify guard for xid ${XID})"
    grep -E "set_visibility xid=${XID}" "${sidecar_log}" 2>/dev/null | sed 's/^/    /'
    exit 1
fi
echo "ICONIFY-SMOKE: sidecar authored Iconic (no Hidden) then restored to Visible over the wire for xid ${XID}"

# (3) The worker registry: stop the sidecar (the worker serves one sidecar connection at a time) and
# prove the full loop LANDED -- the host converged to VISIBLE + NOT iconic at the moved position.
kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
VKR_SIDECAR_PID=""
plane="${VKRELAY2_SIDECAR_PLANE_HOST:-127.0.0.1}:${VKRELAY2_SIDECAR_PLANE_PORT}"
echo "ICONIFY-SMOKE: querying the worker for xid=${XID} (expect visible=1 iconic=0 @ ${TARGET_X},${TARGET_Y})"
if ! "${query}" --connect "${plane}" --sidecar-token "${VKRELAY2_SIDECAR_TOKEN}" \
        --xid "${XID}" --expect-x "${TARGET_X}" --expect-y "${TARGET_Y}" \
        --expect-visible 1 --expect-iconic 0 2>&1 | sed 's/^/    /'; then
    echo "ICONIFY-SMOKE: FAIL (worker did not converge to Visible+not-iconic after minimize->restore)"
    exit 1
fi
echo "ICONIFY-SMOKE: PASS (a Win32-user minimize->restore loop drove Iconic then Visible over the wire)"
