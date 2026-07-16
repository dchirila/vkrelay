#!/usr/bin/env bash
# vkrelay2 boundary smoke: the mapped-vertex-buffer DRAW spine on screen.
#
# Brings up the private weston-headless + rootless-Xwayland display, establishes a worker session,
# pins the vkrelay2 ICD, runs the VBO canary, and asserts the whole chain reached the worker (loader
# picked OUR ICD -> app_ack -> surface/swapchain -> buffer/memory/map -> pipeline -> bind vertex
# buffers -> draw -> present). Also runs the CANARY TRACE INVENTORY: every entrypoint the VBO canary
# imports must be one the ICD exports, and the full memory/vertex spine must be present -- while
# NO / entrypoint (descriptors / images / samplers) appears, proving the canary stays in
# the subset.
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
vbo="$build/vkrelay2-vbo"
manifest="$build/vkrelay2_icd.json"

# shellcheck source=lib_private_session.sh
source "$here/lib_private_session.sh"

fail() { echo "run_vbo: $*" >&2; exit 1; }

for f in "$launch" "$vbo" "$manifest"; do
    [ -e "$f" ] || fail "missing build artifact: $f (build build/linux-debug first)"
done

# --- Canary trace inventory ------------------------------------------------------------------------
# EVERY imported vk* symbol must be one the ICD exports (derived from get_proc's RET()/strcmp set in
# icd.cpp, so the check tracks the ICD), AND the full memory/vertex spine must be present (a
# regression that stops driving the mapped-buffer path is caught). A static guard -- runs even without
# the boundary up.
imported="$(nm -u "$vbo" 2>/dev/null | grep -oE 'vk[A-Za-z0-9]+' | sort -u || true)"
[ -n "$imported" ] || fail "trace inventory: could not read imported vk* symbols from $vbo"
echo "run_vbo: canary trace inventory ($(echo "$imported" | wc -l) entrypoints):" >&2
echo "$imported" | sed 's/^/  /' >&2

icd_src="$here/../icd/icd.cpp"
[ -e "$icd_src" ] || fail "trace inventory: ICD source not found at $icd_src"
supported="$( { grep -oE 'RET\([A-Za-z0-9]+\)' "$icd_src" | sed -E 's/RET\((.+)\)/vk\1/'
               grep -oE '"vk[A-Za-z0-9]+"' "$icd_src" | tr -d '"'; } | sort -u )"
unsupported="$(comm -23 <(echo "$imported") <(echo "$supported"))"
if [ -n "$unsupported" ]; then
    echo "$unsupported" | sed 's/^/  /' >&2
    fail "trace inventory: the VBO canary imports entrypoints the ICD does not export (above)"
fi
# The spine must actually be exercised: the draw subset PLUS the memory/buffer/vertex entrypoints.
for sym in vkCreateImageView vkCreateShaderModule vkCreateRenderPass vkCreateFramebuffer \
           vkCreatePipelineLayout vkCreateGraphicsPipelines vkCmdBeginRenderPass vkCmdEndRenderPass \
           vkCmdBindPipeline vkCmdSetViewport vkCmdSetScissor vkCmdDraw \
           vkGetPhysicalDeviceMemoryProperties vkCreateBuffer vkGetBufferMemoryRequirements \
           vkBindBufferMemory vkAllocateMemory vkMapMemory vkCmdBindVertexBuffers; do
    echo "$imported" | grep -qx "$sym" || fail "trace inventory: expected entrypoint $sym not imported"
done
echo "run_vbo: trace inventory OK (all imports ICD-supported; full memory/vertex spine present)" >&2

trap 'vkr_session_cleanup "$?"' EXIT

vkr_start_private_display || fail "private display setup failed"
vkr_open_session_and_pin_icd "$launch" "$manifest" vkrelay2-vbo \
    || fail "session/ICD setup failed (is the daemon up as '--serve --vulkan-backend real'?)"

log="$VKR_RUNTIME_DIR/vbo.log"
echo "run_vbo: running the VBO canary (VK_ICD_FILENAMES=$manifest)" >&2
rc=0
VK_LOADER_DEBUG=driver "$vbo" >"$log" 2>&1 || rc=$?
cat "$log" >&2

# Assert the draw chain. "data plane connected ... app_ack ok" is printed ONLY by our ICD.
assert() { grep -q "$1" "$log" || fail "chain assertion failed: missing '$1' (see $log)"; }
assert "vkrelay2-icd: data plane connected"
assert "app_ack ok"
assert "vkrelay2-vbo: vertex buffer mapped + written"
assert "vkrelay2-vbo: surface created"
assert "vkrelay2-vbo: swapchain created"
assert "vkrelay2-vbo: graphics pipeline created"
assert "vkrelay2-vbo: presented frame"

[ "$rc" -eq 0 ] || fail "VBO canary exited non-zero ($rc) despite chain markers (see $log)"

echo "run_vbo: PASS -- loader selected the vkrelay2 ICD; mapped-VBO triangle drawn + presented via worker-present" >&2
