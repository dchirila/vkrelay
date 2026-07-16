#!/usr/bin/env bash
# vkrelay2 fullscreen-toplevel smoke (the ExtremeTuxRacer class): a ROOT-SIZED OVERRIDE-REDIRECT
# window -- SFML 2.5's fullscreen shape when no EWMH WM is advertised -- must be REGISTERED as a
# toplevel by the sidecar, not dropped as popup junk. Same bring-up as run_clamp_smoke (private
# rootless-Xwayland + a mock-backend daemon + a session + the real sidecar/WM), then maps
# vkrelay2-fullscreen-canary, which waits for the one event SFML's sf::Window::create busy-waits
# on: a VisibilityNotify with state != FullyObscured. Rootless Xwayland reports an UNREDIRECTED
# window FullyObscured, and only the sidecar's cap_arm composite redirect flips it -- so:
#   - PRE-FIX: the sidecar dropped the window ("popup drop ... root-sized"), nothing redirected
#     it, the canary times out = exactly the silent multi-minute app hang -> FAIL;
#   - FIXED: the sidecar registers + redirects it -> VisibilityNotify Unobscured -> PASS.
# The canary's event readback is the primary proof; the smoke also asserts the sidecar's
# classification decision in its log (fullscreen or-toplevel ... registering).
#
# Mock backend (WSL has no real GPU): the classification + redirect are sidecar/X-side decisions
# (mock == real for them). Skips cleanly (exit 0) when tools / built binaries are absent.
#
# Usage: bash run_fullscreen_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "FULLSCREEN-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-fullscreen-canary"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done

# shellcheck source=lib_private_session.sh
. "${script_dir}/lib_private_session.sh"

DAEMON_PID=""
smoke_cleanup() {
    local ec="${1:-0}"
    [ -n "${DAEMON_PID}" ] && kill "${DAEMON_PID}" 2>/dev/null || true
    vkr_session_cleanup "${ec}"
}
trap 'smoke_cleanup "$?"' EXIT

vkr_reexec_in_private_x11_namespace "$@"

vkr_start_private_display || { echo "FULLSCREEN-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "FULLSCREEN-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "FULLSCREEN-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id fullscreen-smoke)" \
    || { echo "FULLSCREEN-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "FULLSCREEN-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "FULLSCREEN-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}

# Run the canary to completion: it exits 0 on the non-obscured VisibilityNotify (the SFML unblock
# condition) and 1 on its internal deadline (the pre-fix forever-spin, bounded).
canary_log="${VKR_RUNTIME_DIR}/fullscreen_canary.log"
if timeout 60 "${canary}" >"${canary_log}" 2>&1; then canary_rc=0; else canary_rc=$?; fi
sed 's/^/    /' "${canary_log}"

if [ "${canary_rc}" -ne 0 ]; then
    echo "FULLSCREEN-SMOKE: FAIL (canary rc=${canary_rc} -- the root-sized override-redirect window"
    echo "                  never became visible; an SFML 2.5 fullscreen app would hang forever)"
    grep -E "popup drop|fullscreen or-toplevel|register_toplevel" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null | sed 's/^/    /' || true
    exit 1
fi

# Secondary witness: the sidecar's classification decision (registered as a toplevel, not dropped)
# AND the worker's acceptance (applied=1, a placeholder host -- the old worker-side
# refuse-all-non-popup gate stranded the window on the create_surface default host).
if ! grep -q "fullscreen or-toplevel" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null; then
    echo "FULLSCREEN-SMOKE: FAIL (canary passed but the sidecar log carries no fullscreen"
    echo "                  or-toplevel registration -- the visibility flip came from elsewhere?)"
    grep -E "popup drop|register_toplevel" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null | sed 's/^/    /' || true
    exit 1
fi
if ! grep -q "register_toplevel .*applied=1 representation=placeholder" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null; then
    echo "FULLSCREEN-SMOKE: FAIL (the worker did not APPLY the fullscreen-toplevel registration --"
    echo "                  no placeholder host; the window would strand on the 256x256 default)"
    grep -E "register_toplevel" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null | sed 's/^/    /' || true
    exit 1
fi
grep -E "fullscreen or-toplevel|register_toplevel" "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null | sed 's/^/    /' || true

echo "============================================================"
echo "FULLSCREEN-SMOKE: PASS (a root-sized override-redirect window is registered as a fullscreen"
echo "                  toplevel + composite-redirected -- the SFML 2.5 fullscreen class no longer"
echo "                  hangs waiting for a VisibilityNotify that never comes)"
echo "============================================================"
exit 0
