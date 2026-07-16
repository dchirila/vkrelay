#!/usr/bin/env bash
# vkrelay2 boundary smoke: the descriptor / per-frame UBO surface on screen.
#
# Brings up the private weston-headless + rootless-Xwayland display, establishes a worker session,
# pins the vkrelay2 ICD, runs the UBO canary, and asserts the whole chain reached the worker (loader
# picked OUR ICD -> app_ack -> surface/swapchain -> buffer/memory/map -> descriptor set layout/pool/
# set/update -> pipeline -> bind descriptor sets + vertex buffers -> draw -> present). Also runs the
# CANARY TRACE INVENTORY: every entrypoint the UBO canary imports must be one the ICD exports, and the
# full descriptor spine (on top of the memory/vertex spine) must be present.
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
ubo="$build/vkrelay2-ubo"
manifest="$build/vkrelay2_icd.json"

# shellcheck source=lib_private_session.sh
source "$here/lib_private_session.sh"

fail() { echo "run_ubo: $*" >&2; exit 1; }

for f in "$launch" "$ubo" "$manifest"; do
    [ -e "$f" ] || fail "missing build artifact: $f (build build/linux-debug first)"
done

# --- Canary trace inventory ------------------------------------------------------------------------
# EVERY imported vk* symbol must be one the ICD exports (derived from get_proc's RET()/strcmp set in
# icd.cpp, so the check tracks the ICD), AND the full descriptor spine must be present (a
# regression that stops driving the descriptor/UBO path is caught). A static guard -- runs even
# without the boundary up.
imported="$(nm -u "$ubo" 2>/dev/null | grep -oE 'vk[A-Za-z0-9]+' | sort -u || true)"
[ -n "$imported" ] || fail "trace inventory: could not read imported vk* symbols from $ubo"
echo "run_ubo: canary trace inventory ($(echo "$imported" | wc -l) entrypoints):" >&2
echo "$imported" | sed 's/^/  /' >&2

icd_src="$here/../icd/icd.cpp"
[ -e "$icd_src" ] || fail "trace inventory: ICD source not found at $icd_src"
supported="$( { grep -oE 'RET\([A-Za-z0-9]+\)' "$icd_src" | sed -E 's/RET\((.+)\)/vk\1/'
               grep -oE '"vk[A-Za-z0-9]+"' "$icd_src" | tr -d '"'; } | sort -u )"
unsupported="$(comm -23 <(echo "$imported") <(echo "$supported"))"
if [ -n "$unsupported" ]; then
    echo "$unsupported" | sed 's/^/  /' >&2
    fail "trace inventory: the UBO canary imports entrypoints the ICD does not export (above)"
fi
# The spine must actually be exercised: the memory/vertex spine PLUS the descriptor surface.
for sym in vkGetPhysicalDeviceMemoryProperties vkCreateBuffer vkBindBufferMemory vkAllocateMemory \
           vkMapMemory vkCmdBindVertexBuffers vkCreateDescriptorSetLayout vkCreateDescriptorPool \
           vkAllocateDescriptorSets vkUpdateDescriptorSets vkCreatePipelineLayout \
           vkCreateGraphicsPipelines vkCmdBindDescriptorSets vkCmdDraw; do
    echo "$imported" | grep -qx "$sym" || fail "trace inventory: expected entrypoint $sym not imported"
done
echo "run_ubo: trace inventory OK (all imports ICD-supported; full descriptor spine present)" >&2

trap 'vkr_session_cleanup "$?"' EXIT

vkr_start_private_display || fail "private display setup failed"
vkr_open_session_and_pin_icd "$launch" "$manifest" vkrelay2-ubo \
    || fail "session/ICD setup failed (is the daemon up as '--serve --vulkan-backend real'?)"

log="$VKR_RUNTIME_DIR/ubo.log"
echo "run_ubo: running the UBO canary (VK_ICD_FILENAMES=$manifest)" >&2
rc=0
VK_LOADER_DEBUG=driver "$ubo" >"$log" 2>&1 || rc=$?
cat "$log" >&2

# Assert the descriptor/UBO chain. "data plane connected ... app_ack ok" is printed ONLY by our ICD.
assert() { grep -q "$1" "$log" || fail "chain assertion failed: missing '$1' (see $log)"; }
assert "vkrelay2-icd: data plane connected"
assert "app_ack ok"
assert "vkrelay2-ubo: vertex buffer mapped + written"
assert "vkrelay2-ubo: uniform buffer mapped"
assert "vkrelay2-ubo: descriptor set updated"
assert "vkrelay2-ubo: surface created"
assert "vkrelay2-ubo: swapchain created"
assert "vkrelay2-ubo: graphics pipeline created"
# Require EVERY frame to have presented (the animation proof), not just "something presented" -- the
# count line prints the actual total, so this only matches when all 30 frames (== kFrames in
# ubo_canary.cpp) landed.
assert "vkrelay2-ubo: presented 30 frame(s)"

[ "$rc" -eq 0 ] || fail "UBO canary exited non-zero ($rc) despite chain markers (see $log)"

echo "run_ubo: PASS -- loader selected the vkrelay2 ICD; per-frame-UBO spinning triangle drawn + presented via worker-present" >&2
