#!/usr/bin/env bash
# vkrelay2 boundary smoke: the literal-vkcube shape on screen via the in-repo canary.
#
# Brings up the private weston-headless + rootless-Xwayland display, establishes a worker session,
# pins the vkrelay2 ICD, runs the vkrelay2-cube canary, and asserts the whole chain reached the
# worker (loader picked OUR ICD -> app_ack -> format-properties path choice -> texture image/memory/
# upload + sampler -> depth image -> 2-binding descriptor set -> depth render pass -> bufferless
# draw(36) -> present). Also runs the CANARY TRACE INVENTORY: every entrypoint the cube canary
# imports must be one the ICD exports, and the full spine (texture + sampler + depth on top of
# the descriptor spine) must be present.
#
# This is the DETERMINISTIC on-screen proof (our checked-in cube_spv.h / fixed geometry). The literal
# public gate -- LunarG's actual vkcube -- is run_vkcube.sh; this canary smoke does not replace it.
#
# Opt-in / manual: NOT a ctest target -- the on-screen proof is a WSL->Windows boundary run and needs
# the Windows daemon + worker up. Shares the display/session/ICD setup with the app-run path via
# lib_private_session.sh (fail-closed: never falls through to a system driver).
#
# Prerequisites (Windows side, started by the user):
#   vkrelay2-supervisor.exe --serve --vulkan-backend real
# Linux side: weston + Xwayland + libxcb; a built build/linux-debug (override with VKRELAY2_BUILD).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${VKRELAY2_BUILD:-$here/../../build/linux-debug}"
launch="$build/vkrelay2-launch"
cube="$build/vkrelay2-cube"
manifest="$build/vkrelay2_icd.json"

# shellcheck source=lib_private_session.sh
source "$here/lib_private_session.sh"

fail() { echo "run_cube: $*" >&2; exit 1; }

for f in "$launch" "$cube" "$manifest"; do
    [ -e "$f" ] || fail "missing build artifact: $f (build build/linux-debug first)"
done

# --- Canary trace inventory ------------------------------------------------------------------------
# EVERY imported vk* symbol must be one the ICD exports (derived from get_proc's RET()/strcmp set in
# icd.cpp, so the check tracks the ICD), AND the full spine must be present (a regression that
# stops driving the texture/depth path is caught). A static guard -- runs even without the boundary up.
imported="$(nm -u "$cube" 2>/dev/null | grep -oE 'vk[A-Za-z0-9]+' | sort -u || true)"
[ -n "$imported" ] || fail "trace inventory: could not read imported vk* symbols from $cube"
echo "run_cube: canary trace inventory ($(echo "$imported" | wc -l) entrypoints):" >&2
echo "$imported" | sed 's/^/  /' >&2

icd_src="$here/../icd/icd.cpp"
[ -e "$icd_src" ] || fail "trace inventory: ICD source not found at $icd_src"
supported="$( { grep -oE 'RET\([A-Za-z0-9]+\)' "$icd_src" | sed -E 's/RET\((.+)\)/vk\1/'
               grep -oE '"vk[A-Za-z0-9]+"' "$icd_src" | tr -d '"'; } | sort -u )"
unsupported="$(comm -23 <(echo "$imported") <(echo "$supported"))"
if [ -n "$unsupported" ]; then
    echo "$unsupported" | sed 's/^/  /' >&2
    fail "trace inventory: the cube canary imports entrypoints the ICD does not export (above)"
fi
# The spine must actually be exercised: the texture (image/memory/copy/sampler), the depth image,
# the format-properties path choice, and the descriptor spine.
for sym in vkGetPhysicalDeviceFormatProperties vkCreateImage vkBindImageMemory \
           vkGetImageMemoryRequirements vkCreateSampler vkCmdPipelineBarrier \
           vkCreateDescriptorSetLayout vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw; do
    echo "$imported" | grep -qx "$sym" || fail "trace inventory: expected entrypoint $sym not imported"
done
echo "run_cube: trace inventory OK (all imports ICD-supported; full texture/depth spine present)" >&2

trap 'vkr_session_cleanup "$?"' EXIT

vkr_start_private_display || fail "private display setup failed"
vkr_open_session_and_pin_icd "$launch" "$manifest" vkrelay2-cube \
    || fail "session/ICD setup failed (is the daemon up as '--serve --vulkan-backend real'?)"

log="$VKR_RUNTIME_DIR/cube.log"
echo "run_cube: running the cube canary (VK_ICD_FILENAMES=$manifest)" >&2
rc=0
VK_LOADER_DEBUG=driver "$cube" >"$log" 2>&1 || rc=$?
cat "$log" >&2

# Assert the texture/depth/cube chain. "data plane connected ... app_ack ok" is printed ONLY by our ICD.
assert() { grep -q "$1" "$log" || fail "chain assertion failed: missing '$1' (see $log)"; }
assert "vkrelay2-icd: data plane connected"
assert "app_ack ok"
assert "vkrelay2-cube: texture + sampler created"
assert "vkrelay2-cube: depth image + view created"
assert "vkrelay2-cube: descriptor set updated (UBO + combined-image-sampler)"
assert "vkrelay2-cube: texture uploaded to SHADER_READ_ONLY"
assert "vkrelay2-cube: graphics pipeline created"
assert "vkrelay2-cube: swapchain created"
# Require EVERY frame to have presented (the spin proof), not just "something presented" -- the count
# line prints the actual total, so this only matches when all 60 frames (== kFrames in cube_canary.cpp)
# landed.
assert "vkrelay2-cube: presented 60 frame(s)"

[ "$rc" -eq 0 ] || fail "cube canary exited non-zero ($rc) despite chain markers (see $log)"

echo "run_cube: PASS -- loader selected the vkrelay2 ICD; bufferless spinning textured depth cube drawn + presented via worker-present" >&2
