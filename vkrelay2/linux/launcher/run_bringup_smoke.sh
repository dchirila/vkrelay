#!/usr/bin/env bash
# vkrelay2 launcher/session-reliability boundary smoke.
#
# The user hit an INTERMITTENT hang of `vkrun -- openscad`. makes the control-plane
# bring-up fail CLOSED instead of hanging (per-read deadlines in the C++ client + supervisor serve
# loop, a protocol-level --ping health check, and a bounded --open-session). This smoke is the
# repeated-bring-up regression: it brings up ONE private display + ONE mock-backend daemon, then runs
# the full bring-up N times against that single long-lived daemon -- each iteration must, WITHIN A
# DEADLINE, (1) pass the --ping handshake health check, (2) complete --open-session, (3) reach
# sidecar-ready, and (4) register a mapped X toplevel with the worker. If the daemon's one-at-a-time
# serve loop could be wedged by an earlier session (the old bug), a later iteration would block past
# its deadline and this FAILS loudly instead of hanging forever. The first failing iteration's full
# log bundle (incl. phase.log) is preserved via VKRELAY2_LOG_DIR.
#
# A lighter X canary than OpenSCAD (vkrelay2-placement-canary, pure xcb) keeps it fast + GPU-free; the
# property under test is control-plane liveness, not GL. Mock backend (WSL has no real GPU). Skips
# cleanly (exit 0) when the private-display tools / built binaries are absent.
#
# Usage: bash run_bringup_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
# Env:   VKRELAY2_BRINGUP_ITERS (default 5)   VKRELAY2_BRINGUP_DEADLINE seconds (default 25)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"
iters="${VKRELAY2_BRINGUP_ITERS:-5}"
deadline="${VKRELAY2_BRINGUP_DEADLINE:-25}"

skip() { echo "BRINGUP-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-placement-canary"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# shellcheck source=lib_private_session.sh
. "${script_dir}/lib_private_session.sh"

DAEMON_PID=""
DAEMON_PGID="" # set only when the daemon runs in its own process group (setsid), so we can group-kill
CANARY_PID=""
# Preserve the failing bundle (phase.log + sidecar/daemon logs) automatically. Track whether WE
# defaulted the dir so a GREEN run can drop it (no litter) while an explicit user VKRELAY2_LOG_DIR is
# always respected (the EXIT-trap cleanup would otherwise recreate the bundle on success).
# Decide ownership ONCE and export the decision: the WSLg /tmp/.X11-unix namespace re-exec
# (vkr_reexec_in_private_x11_namespace, below) restarts this script with our exported VKRELAY2_LOG_DIR
# already set, so a naive "was it set at entry?" check would mistake OUR dir for a caller's on the second
# pass and never clean up. The marker survives the re-exec so both passes agree.
if [ -z "${VKR_BRINGUP_LOG_OWNED:-}" ]; then
    if [ -n "${VKRELAY2_LOG_DIR:-}" ]; then
        export VKR_BRINGUP_LOG_OWNED=0 # caller provided it -- always respect, never auto-drop
    else
        export VKRELAY2_LOG_DIR="/tmp/vkrelay2-bringup-smoke-$$"
        export VKR_BRINGUP_LOG_OWNED=1
    fi
fi
owns_log_dir="${VKR_BRINGUP_LOG_OWNED}"

smoke_cleanup() {
    local ec="${1:-0}"
    [ -n "${CANARY_PID}" ] && kill "${CANARY_PID}" 2>/dev/null || true
    # Take the daemon AND its spawned (leaked-idle) workers down together when we own the group.
    if [ -n "${DAEMON_PGID}" ]; then
        kill -- -"${DAEMON_PGID}" 2>/dev/null || true
    elif [ -n "${DAEMON_PID}" ]; then
        kill "${DAEMON_PID}" 2>/dev/null || true
    fi
    vkr_session_cleanup "${ec}"
}
trap 'smoke_cleanup "$?"' EXIT
trap 'smoke_cleanup 143; exit 143' TERM
trap 'smoke_cleanup 130; exit 130' INT

vkr_reexec_in_private_x11_namespace "$@" || exit $?

# Begin the phase trace up front so a stall is localized in the preserved bundle.
VKR_PHASE_LOG="$(mktemp "${TMPDIR:-/tmp}/vkrelay2-phase-XXXXXX.log" 2>/dev/null)" || VKR_PHASE_LOG=""
export VKR_PHASE_LOG
vkr_phase "bringup-smoke: starting (${iters} iterations, ${deadline}s deadline each)"

vkr_start_private_display || { echo "BRINGUP-SMOKE: FAIL (private display setup)"; exit 1; }

# One long-lived mock-backend daemon, in its OWN process group so cleanup reaps its leaked workers.
port_file="${VKR_RUNTIME_DIR}/daemon.port"
if command -v setsid >/dev/null 2>&1; then
    setsid "${supervisor}" --serve --port 0 --port-file "${port_file}" --worker "${worker}" \
        --vulkan-backend mock >"${VKR_RUNTIME_DIR}/daemon.log" 2>&1 &
    DAEMON_PID=$!
    DAEMON_PGID="${DAEMON_PID}" # setsid execs in place -> $! is the new session/group leader
else
    "${supervisor}" --serve --port 0 --port-file "${port_file}" --worker "${worker}" \
        --vulkan-backend mock >"${VKR_RUNTIME_DIR}/daemon.log" 2>&1 &
    DAEMON_PID=$!
fi
ok=""
for _ in $(seq 1 50); do
    [ -s "${port_file}" ] && { ok=1; break; }
    kill -0 "${DAEMON_PID}" 2>/dev/null || break
    sleep 0.1
done
[ -n "${ok}" ] || { echo "BRINGUP-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "BRINGUP-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"
vkr_phase "bringup-smoke: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"
canary_log="${VKR_RUNTIME_DIR}/bringup_canary.log"

iter_fail() {
    echo "BRINGUP-SMOKE: FAIL (iteration $1: $2)"
    vkr_phase "bringup-smoke: FAIL iteration $1: $2"
    exit 1
}

i=1
while [ "${i}" -le "${iters}" ]; do
    iter_t0="${SECONDS}"
    vkr_phase "iter ${i}: start"

    # (1) Protocol-level health check must answer within the deadline (a wedged loop fails here).
    timeout "${deadline}" "${launch}" --ping >/dev/null 2>&1 \
        || iter_fail "${i}" "--ping handshake health check did not answer within ${deadline}s"

    # (2) --open-session must complete within the deadline (the old hang point). rc 124 == timed out.
    session_env="$(timeout "${deadline}" "${launch}" --open-session --app-id "bringup-smoke-${i}")" \
        && oss_rc=0 || oss_rc=$?
    if [ "${oss_rc}" -ne 0 ]; then
        [ "${oss_rc}" -eq 124 ] \
            && iter_fail "${i}" "--open-session did not complete within ${deadline}s (control loop wedged?)"
        iter_fail "${i}" "--open-session failed (rc=${oss_rc})"
    fi
    while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
    [ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
        || iter_fail "${i}" "session carried no sidecar plane"

    # (3) Sidecar must reach readiness. It runs in THIS shell (it sets VKR_SIDECAR_PID + holds the
    # ready fd, so it cannot be backgrounded or `timeout`-wrapped); its own readiness barrier is
    # already bounded (an internal `read -t`), so a stuck sidecar returns non-zero rather than hangs.
    : >"${sidecar_log}" 2>/dev/null || true
    vkr_start_sidecar_and_wait_ready "${sidecar}" \
        || iter_fail "${i}" "sidecar did not become ready"

    # (4) Map a toplevel; the sidecar must REGISTER it with the worker within the deadline.
    : >"${canary_log}" 2>/dev/null || true
    DISPLAY="${DISPLAY}" "${canary}" >"${canary_log}" 2>&1 &
    CANARY_PID=$!
    XID=""
    for _ in $(seq 1 50); do
        XID="$(sed -n 's/^PLACEMENT-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" 2>/dev/null | head -1)"
        [ -n "${XID}" ] && break
        kill -0 "${CANARY_PID}" 2>/dev/null || break
        sleep 0.1
    done
    [ -n "${XID}" ] || iter_fail "${i}" "canary did not report its xid"

    registered=""
    deadline_ticks=$((deadline * 10))
    for _ in $(seq 1 "${deadline_ticks}"); do
        if grep -q "register_toplevel xid=${XID} " "${sidecar_log}" 2>/dev/null; then
            registered=1
            break
        fi
        sleep 0.1
    done
    [ -n "${registered}" ] || iter_fail "${i}" "worker never registered toplevel xid=${XID} within ${deadline}s"

    # Tear down this iteration's canary + sidecar before the next (each iteration is a fresh session).
    kill "${CANARY_PID}" 2>/dev/null || true
    CANARY_PID=""
    [ -n "${VKR_SIDECAR_PID:-}" ] && kill "${VKR_SIDECAR_PID}" 2>/dev/null || true
    VKR_SIDECAR_PID=""

    echo "BRINGUP-SMOKE: iter ${i}/${iters} OK (xid=${XID}, $((SECONDS - iter_t0))s)"
    vkr_phase "iter ${i}: OK ($((SECONDS - iter_t0))s)"
    i=$((i + 1))
done

# Everything passed -> if WE created the bundle dir, drop it AND unset VKRELAY2_LOG_DIR so the EXIT-trap
# cleanup does not recreate it (no litter on a green run). An explicit user dir is left untouched.
if [ "${owns_log_dir}" = "1" ]; then
    rm -rf "${VKRELAY2_LOG_DIR}" 2>/dev/null || true
    unset VKRELAY2_LOG_DIR VKR_BRINGUP_LOG_OWNED
fi

echo "============================================================"
echo "BRINGUP-SMOKE: PASS (${iters} consecutive bring-ups, each within ${deadline}s -- the control"
echo "                loop kept serving session-after-session and never wedged)"
echo "============================================================"
exit 0
