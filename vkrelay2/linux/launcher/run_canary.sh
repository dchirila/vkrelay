#!/usr/bin/env bash
# vkrelay2 boundary smoke (part 2).
#
# Brings up a private weston-headless + rootless-Xwayland display, establishes a worker session,
# pins the vkrelay2 ICD, runs the clear canary, and asserts the whole chain reached the worker
# (loader picked OUR ICD -> data plane app_ack -> surface/swapchain -> clear -> present). Opt-in
# This is an opt-in manual test, not a ctest target: the on-screen proof needs a WSL-to-Windows
# boundary run with the Windows daemon and worker up. The display/session/ICD setup is shared with
# the app-run path
# via lib_private_session.sh (the launcher establishes session+display before launching, or
# fails -- it never falls through to a system driver).
#
# Sidecar note: this is a LOWER-LEVEL, pre-sidecar worker-present smoke -- it proves the
# GPU present path and deliberately does NOT start the sidecar / readiness barrier. The sidecar
# boundary is covered by run_sidecar_smoke.sh; the full production app-run path (which starts + waits
# for the sidecar before launch) is vkrun.
#
# Prerequisites (Windows side, started by the user):
#   vkrelay2-supervisor.exe --serve --vulkan-backend real   (the daemon must launch REAL workers;
#   the session setup below fails closed against a mock daemon -- a mock worker shows no real window)
# Linux side: weston + Xwayland + libxcb; a built build/linux-debug
#   (override with VKRELAY2_BUILD).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${VKRELAY2_BUILD:-$here/../../build/linux-debug}"
launch="$build/vkrelay2-launch"
canary="$build/vkrelay2-canary"
manifest="$build/vkrelay2_icd.json"

# shellcheck source=lib_private_session.sh
source "$here/lib_private_session.sh"

fail() { echo "run_canary: $*" >&2; exit 1; }

for f in "$launch" "$canary" "$manifest"; do
    [ -e "$f" ] || fail "missing build artifact: $f (build build/linux-debug first)"
done

trap 'vkr_session_cleanup "$?"' EXIT

vkr_start_private_display || fail "private display setup failed"
vkr_open_session_and_pin_icd "$launch" "$manifest" vkrelay2-canary \
    || fail "session/ICD setup failed (is the daemon up as '--serve --vulkan-backend real'?)"

log="$VKR_RUNTIME_DIR/canary.log"
echo "run_canary: running the clear canary (VK_ICD_FILENAMES=$manifest)" >&2
rc=0
VK_LOADER_DEBUG=driver "$canary" >"$log" 2>&1 || rc=$?
cat "$log" >&2

# Assert the chain. The ICD's "data plane connected ... app_ack ok" is printed ONLY
# by our ICD: seeing it proves the loader selected libvulkan_vkrelay2.so (not a system driver /
# the mock) AND the data-plane handshake succeeded. The rest proves the worker served the frame.
assert() { grep -q "$1" "$log" || fail "chain assertion failed: missing '$1' (see $log)"; }
assert "vkrelay2-icd: data plane connected"
assert "app_ack ok"
assert "vkrelay2-canary: surface created"
assert "vkrelay2-canary: swapchain created"
assert "vkrelay2-canary: presented frame"

# A non-zero canary exit is a failure even if the markers printed (the chain must complete AND
# the process must exit cleanly -- e.g. a crash during teardown is a real defect).
[ "$rc" -eq 0 ] || fail "canary exited non-zero ($rc) despite chain markers (see $log)"

echo "run_canary: PASS -- loader selected the vkrelay2 ICD; clear frame presented via worker-present" >&2
