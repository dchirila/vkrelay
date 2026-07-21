#!/usr/bin/env bash
# vkrelay2 LITERAL on-screen gate: LunarG's actual vkcube on the Windows desktop via the
# relay.
#
# This is the public "vkcube on screen" proof -- it drives the REAL upstream vkcube binary (its own
# shaders/texture/geometry, NOT our checked-in cube_spv.h) through the loader with the vkrelay2 ICD
# pinned, so a clean run means app compatibility is genuinely proven, not just a vkcube-shaped clone.
# The deterministic regression counterpart is the in-repo vkrelay2-cube canary (run_cube.sh) +
# integration_real_cube; this script is the thing that lets us truthfully say "vkcube renders through
# vkrelay2."
#
# Opt-in / manual: NOT a ctest target -- the on-screen proof is a WSL->Windows boundary run and needs
# the Windows daemon + worker up. Shares the display/session/ICD setup with the canary smokes via
# lib_private_session.sh (fail-closed: never falls through to a system driver). If the upstream vkcube
# binary is not installed, this FAILS with a clear message rather than silently downgrading to the
# in-repo canary -- the canary alone does not close this gate.
#
# Sidecar note: this is a LOWER-LEVEL, pre-sidecar worker-present gate -- it proves the
# GPU present path for real vkcube and deliberately does NOT start the sidecar / readiness barrier.
# The sidecar boundary is covered by run_sidecar_smoke.sh; the full production app-run path (which
# starts + waits for the sidecar before launch) is vkrun -- vkcube.
#
# Prerequisites (Windows side, started by the user):
#   vkrelay2-supervisor.exe --serve --vulkan-backend real
# Linux side: weston + Xwayland + libxcb; a built build/linux-debug (override with VKRELAY2_BUILD);
# and the real vkcube binary (Vulkan-Tools / vulkan-tools package, or set VKRELAY2_VKCUBE).
#
# Env overrides:
#   VKRELAY2_VKCUBE         path to the real vkcube binary (else PATH + common locations are searched)
#   VKRELAY2_VKCUBE_FRAMES  frames to run before vkcube exits (default 120; vkcube's --c flag)
#   VKRELAY2_BUILD          the Linux build dir (default build/linux-debug)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${VKRELAY2_BUILD:-$here/../../build/linux-debug}"
launch="$build/vkrelay2-launch"
manifest="$build/vkrelay2_icd.json"
frames="${VKRELAY2_VKCUBE_FRAMES:-60}"
# vkcube runs at its NATIVE default extent (the worker-present extent negotiation now honors an
# arbitrary app-chosen size: the worker resizes its host window to whatever vkcube picks). Set
# VKRELAY2_VKCUBE_EXTENT only to force a specific square size (otherwise vkcube chooses).
extent="${VKRELAY2_VKCUBE_EXTENT:-}"

# shellcheck source=lib_private_session.sh
source "$here/lib_private_session.sh"

# On a WSLg box /tmp/.X11-unix is read-only; transparently re-exec inside a private mount namespace
# with a writable overlay first (no sudo, no global change). No-op when not needed.
vkr_reexec_in_private_x11_namespace "$@" || exit $?

fail() { echo "run_vkcube: $*" >&2; exit 1; }

for f in "$launch" "$manifest"; do
    [ -e "$f" ] || fail "missing build artifact: $f (build build/linux-debug first)"
done

# Locate the REAL upstream vkcube. Fail closed (do NOT fall back to the in-repo canary) so this gate
# only ever passes on the genuine app.
vkcube="${VKRELAY2_VKCUBE:-}"
if [ -z "$vkcube" ]; then
    for cand in vkcube vkcube-wayland /usr/bin/vkcube /usr/local/bin/vkcube; do
        if command -v "$cand" >/dev/null 2>&1; then
            vkcube="$(command -v "$cand")"
            break
        fi
    done
fi
[ -n "$vkcube" ] && [ -x "$vkcube" ] \
    || fail "the real vkcube binary was not found. Install it (e.g. the 'vulkan-tools' package) or set
  VKRELAY2_VKCUBE=/path/to/vkcube. This is the literal on-screen gate -- it does NOT downgrade to the
  in-repo vkrelay2-cube canary."
# Forced square extent only if VKRELAY2_VKCUBE_EXTENT is set; otherwise vkcube picks its native default
# and the worker resizes its window to match.
extent_args=()
[ -n "$extent" ] && extent_args=(--width "$extent" --height "$extent")
echo "run_vkcube: using upstream vkcube at $vkcube (extent=${extent:-native}, $frames frames)" >&2

trap 'vkr_session_cleanup "$?"' EXIT

vkr_start_private_display || fail "private display setup failed"
vkr_open_session_and_pin_icd "$launch" "$manifest" vkrelay2-vkcube \
    || fail "session/ICD setup failed (is the daemon up as '--serve --vulkan-backend real'?)"

log="$VKR_RUNTIME_DIR/vkcube.log"
echo "run_vkcube: running vkcube (VK_ICD_FILENAMES=$manifest)" >&2
rc=0
# vkcube's --c N runs N frames then exits cleanly (non-interactive). --suppress_popups avoids any
# blocking dialog on init failure. By default no --width/--height: vkcube renders at its native extent
# and the worker resizes its window to match. VK_LOADER_DEBUG=driver records the selected ICD.
VK_LOADER_DEBUG=driver "$vkcube" --c "$frames" "${extent_args[@]+"${extent_args[@]}"}" \
    --suppress_popups >"$log" 2>&1 || rc=$?
cat "$log" >&2

# Assert the loader selected OUR ICD and the data plane reached the worker -- these two lines are
# printed ONLY by the vkrelay2 ICD, so they cannot appear if vkcube ran on a system driver. That is
# what makes this a relay proof rather than "vkcube happened to run on this box."
assert() { grep -q "$1" "$log" || fail "chain assertion failed: missing '$1' (see $log)"; }
assert "vkrelay2-icd: data plane connected"
assert "app_ack ok"

[ "$rc" -eq 0 ] || fail "vkcube exited non-zero ($rc) -- see $log (it reached our ICD but did not run \
clean to $frames frames)"

echo "run_vkcube: PASS -- LunarG vkcube rendered $frames frames through the vkrelay2 ICD and was \
presented via worker-present on the Windows desktop. The literal 'vkcube on screen' gate is MET." >&2
