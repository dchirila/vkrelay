#!/usr/bin/env bash
# vkrelay2 boundary smoke: the ICD surface-caps cache CONVERGES on resize, end to
# end. Same bring-up as run_resize_smoke (private rootless-Xwayland + a MOCK-backend daemon + a
# session + the real sidecar/WM), but the canary is a REAL Vulkan client through the loader +
# pinned vkrelay2 ICD -- the first smoke to drive the ICD itself against the mock backend, because
# the object under test (the ICD's caps cache, icd_caps_cache.h) lives in the ICD .so where no
# in-process integration test can reach it.
#
# vkrelay2-caps-canary: bring-up at size A -> swapchain -> steady presents + per-frame caps polls
# (the polls the cache serves) -> its OWN ConfigureRequest resize A->B -> the sidecar-authored
# resize latches the mock surface geometry-dirty + pins the authored extent B -> acquire
# returns OUT_OF_DATE over the wire (the honest result signal) -> THE ASSERTION: the FIRST caps
# query after that first non-success result observes B (the cache invalidated + re-queried; a
# never-invalidate cache build fails exactly here) -> swapchain recreate at B -> presents flow.
#
# On top of the canary's self-judged PASS, this smoke asserts the cache DEMONSTRABLY worked via
# the ICD trace counters (VKRELAY2_ICD_TRACE=1 dumps "caps-cache: hits=H misses=M invalidations=I"
# at DestroyInstance): hits >= 1 (it served) and invalidations >= 1 (it invalidated).
#
# Skips cleanly (exit 0) when tools / built binaries are absent (not a WSL display box).
#
# Usage: bash run_caps_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "CAPS-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
supervisor="${build_dir}/vkrelay2-supervisor"
worker="${build_dir}/vkrelay2-worker"
launch="${build_dir}/vkrelay2-launch"
sidecar="${build_dir}/vkrelay2-sidecar"
canary="${build_dir}/vkrelay2-caps-canary"
manifest="${build_dir}/vkrelay2_icd.json"
for b in "${supervisor}" "${worker}" "${launch}" "${sidecar}" "${canary}"; do
    [ -x "${b}" ] || skip "missing binary ${b} (build the linux preset first)"
done
[ -e "${manifest}" ] || skip "missing ICD manifest ${manifest}"

# shellcheck source=lib_private_session.sh
. "${script_dir}/lib_private_session.sh"

DAEMON_PID=""
smoke_cleanup() {
    local ec="${1:-0}"
    [ -n "${DAEMON_PID}" ] && kill "${DAEMON_PID}" 2>/dev/null || true
    vkr_session_cleanup "${ec}"
}
trap 'smoke_cleanup "$?"' EXIT

vkr_reexec_in_private_x11_namespace "$@" || exit $?

vkr_start_private_display || { echo "CAPS-SMOKE: FAIL (private display setup)"; exit 1; }

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
[ -n "${ok}" ] || { echo "CAPS-SMOKE: FAIL (daemon did not report a port)"; tail -20 "${VKR_RUNTIME_DIR}/daemon.log" 2>/dev/null; exit 1; }
export VKRELAY2_DAEMON_PORT="$(cat "${port_file}")"
export VKRELAY2_DAEMON_HOST=127.0.0.1
echo "CAPS-SMOKE: daemon up on 127.0.0.1:${VKRELAY2_DAEMON_PORT}"

session_env="$("${launch}" --open-session --app-id caps-smoke)" \
    || { echo "CAPS-SMOKE: FAIL (could not open a session)"; exit 1; }
while IFS= read -r line; do [ -n "${line}" ] && export "${line?}"; done <<< "${session_env}"
[ -n "${VKRELAY2_SIDECAR_PLANE_PORT:-}" ] \
    || { echo "CAPS-SMOKE: FAIL (session carried no sidecar plane)"; exit 1; }

vkr_start_sidecar_and_wait_ready "${sidecar}" || {
    echo "CAPS-SMOKE: FAIL (sidecar did not become ready)"
    sed 's/^/    /' "${VKR_RUNTIME_DIR}/sidecar.log" 2>/dev/null || true
    exit 1
}
sidecar_log="${VKR_RUNTIME_DIR}/sidecar.log"

# Run the Vulkan canary through the loader with OUR ICD pinned. Bounded (the canary's own signal
# wait is ~10s; bring-up is local). Output goes straight to a file -- no pipe -- so the ICD trace
# (stderr) survives however the run ends.
canary_log="${VKR_RUNTIME_DIR}/caps_canary.log"
rc=0
VK_ICD_FILENAMES="${manifest}" VK_DRIVER_FILES="${manifest}" NODEVICE_SELECT=1 \
    VKRELAY2_ICD_TRACE=1 timeout 60 "${canary}" >"${canary_log}" 2>&1 || rc=$?
grep -E "CAPS-CANARY:" "${canary_log}" | sed 's/^/    /' || true

if grep -q "CAPS-CANARY: SKIP" "${canary_log}"; then
    echo "CAPS-SMOKE: FAIL (canary skipped inside a fully-provisioned smoke -- bring-up broke)"
    exit 1
fi
if ! grep -q "CAPS-CANARY: PASS" "${canary_log}"; then
    echo "CAPS-SMOKE: FAIL (canary rc=${rc} -- see the CAPS-CANARY lines above; full log: ${canary_log})"
    exit 1
fi
[ "${rc}" -eq 0 ] || { echo "CAPS-SMOKE: FAIL (canary rc=${rc} despite PASS line)"; exit 1; }

# The sidecar must have FORWARDED the canary's guest resize (the authored-resize signal path).
XID="$(sed -n 's/^CAPS-CANARY: xid=\([0-9]\+\).*/\1/p' "${canary_log}" | head -1)"
[ -n "${XID}" ] || { echo "CAPS-SMOKE: FAIL (canary never reported its xid)"; exit 1; }
grep -q "update_toplevel xid=${XID} " "${sidecar_log}" || {
    echo "CAPS-SMOKE: FAIL (sidecar never forwarded an update_toplevel for xid ${XID})"
    grep -E "register_toplevel|update_toplevel" "${sidecar_log}" 2>/dev/null | sed 's/^/    /' || true
    exit 1
}

# The cache demonstrably served AND invalidated (the ICD trace counters at DestroyInstance).
counters="$(grep -o "caps-cache: hits=[0-9]* misses=[0-9]* invalidations=[0-9]*" "${canary_log}" | head -1)"
[ -n "${counters}" ] || { echo "CAPS-SMOKE: FAIL (no caps-cache counter line in the ICD trace)"; exit 1; }
hits="$(printf '%s' "${counters}" | sed -n 's/.*hits=\([0-9]*\).*/\1/p')"
invals="$(printf '%s' "${counters}" | sed -n 's/.*invalidations=\([0-9]*\).*/\1/p')"
echo "CAPS-SMOKE: ${counters}"
[ "${hits:-0}" -ge 1 ] || { echo "CAPS-SMOKE: FAIL (cache never served a hit)"; exit 1; }
[ "${invals:-0}" -ge 1 ] || { echo "CAPS-SMOKE: FAIL (cache never invalidated)"; exit 1; }

echo "============================================================"
echo "CAPS-SMOKE: PASS (steady caps polls served from the ICD cache; the guest resize's honest"
echo "            OUT_OF_DATE invalidated it; the FIRST caps query after the signal observed the"
echo "            new extent; swapchain recreated and presents flowed -- ${counters})"
echo "============================================================"
exit 0
