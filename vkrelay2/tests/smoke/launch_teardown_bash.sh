#!/usr/bin/env bash
# app-lifetime-teardown launcher smoke: prove run_managed_child (the launcher-owns-the-app wiring)
# kills the app process GROUP when the launcher is signalled, even for an app that IGNORES signals --
# forcing the full SIGTERM->grace->SIGKILL escalation. This pins the wiring itself (block + sigwait
# thread + own-group spawn), not just Process::terminate_graceful.
#
# Deterministic + daemon-free: VKRELAY2_NO_DAEMON=1 and --run just spawn the app (no daemon/GPU). The
# app runs in its OWN process group (new_group), so killing the group cannot touch this script.
#
# Usage: launch_teardown_bash.sh <path-to-vkrelay2-launch>
set -u

launch="${1:-}"
if [ ! -x "${launch}" ]; then
    echo "LAUNCH-TEARDOWN-SMOKE: SKIP (no vkrelay2-launch at '${launch}')"
    exit 0
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/vkr_teardown.XXXXXX")" || {
    echo "LAUNCH-TEARDOWN-SMOKE: SKIP (mktemp failed)"
    exit 0
}
ready="${tmp}/ready"
marker="vkr_teardown_marker_$$_${RANDOM}"

cleanup() {
    pkill -9 -f "${marker}" 2>/dev/null
    kill -9 "${launch_pid:-}" 2>/dev/null
    rm -rf "${tmp}"
}
trap cleanup EXIT

# The app: ignore INT/TERM, announce readiness, then loop forever on short sleeps so ONLY SIGKILL
# stops it (the sleep child dies on the group SIGTERM but the shell ignores it and respawns) -- this
# forces the grace-then-SIGKILL escalation. The marker makes the process findable + unique.
VKRELAY2_NO_DAEMON=1 "${launch}" --run -- \
    sh -c "trap '' INT TERM; : ${marker}; : >'${ready}'; while :; do sleep 1; done" \
    >/dev/null 2>&1 &
launch_pid=$!

# Bounded wait for the app to be ready.
for _ in $(seq 1 50); do
    [ -f "${ready}" ] && break
    sleep 0.1
done
if [ ! -f "${ready}" ]; then
    echo "LAUNCH-TEARDOWN-SMOKE: FAIL (app never signalled ready)"
    exit 1
fi
if ! pgrep -f "${marker}" >/dev/null 2>&1; then
    echo "LAUNCH-TEARDOWN-SMOKE: FAIL (app process not found after ready)"
    exit 1
fi

# Signal the LAUNCHER. run_managed_child must terminate the app group despite the app ignoring SIGTERM
# (escalating to SIGKILL after the grace).
kill -TERM "${launch_pid}" 2>/dev/null

# Bounded poll (grace is ~2 s) for the whole app group to disappear.
gone=0
for _ in $(seq 1 80); do
    if ! pgrep -f "${marker}" >/dev/null 2>&1; then
        gone=1
        break
    fi
    sleep 0.1
done

if [ "${gone}" -eq 1 ]; then
    echo "LAUNCH-TEARDOWN-SMOKE: PASS (launcher signal killed the signal-ignoring app group)"
    exit 0
fi
echo "LAUNCH-TEARDOWN-SMOKE: FAIL (app survived the launcher teardown -- would leak the worker/HWND)"
exit 1
