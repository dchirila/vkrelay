#!/usr/bin/env bash
# vkrelay2 boundary smoke: the bufferless-triangle DRAW spine on screen.
#
# Brings up the private weston-headless + rootless-Xwayland display, establishes a worker session,
# pins the vkrelay2 ICD, runs the triangle canary, and asserts the whole draw chain reached the
# worker (loader picked OUR ICD -> app_ack -> surface/swapchain -> image views/shaders/render
# pass/pipeline -> draw -> present). Also runs the CANARY TRACE INVENTORY: the entrypoints the
# triangle imports must be a subset of the ICD's supported set, and must NOT include any
# (memory/buffer/descriptor) entrypoint -- proving the triangle is genuinely bufferless.
#
# Opt-in / manual: NOT a ctest target -- the on-screen proof is a WSL->Windows boundary run and
# needs the Windows daemon + worker up. Shares the display/session/ICD setup with the app-run path
# via lib_private_session.sh (fail-closed: never falls through to a system driver).
#
# Prerequisites (Windows side, started by the user):
#   vkrelay2-supervisor.exe --serve --vulkan-backend real
# Linux side: weston + Xwayland + libxcb; a built build/linux-debug
#   (override with VKRELAY2_BUILD).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${VKRELAY2_BUILD:-$here/../../build/linux-debug}"
launch="$build/vkrelay2-launch"
triangle="$build/vkrelay2-triangle"
manifest="$build/vkrelay2_icd.json"

# shellcheck source=lib_private_session.sh
source "$here/lib_private_session.sh"

fail() { echo "run_triangle: $*" >&2; exit 1; }

for f in "$launch" "$triangle" "$manifest"; do
    [ -e "$f" ] || fail "missing build artifact: $f (build build/linux-debug first)"
done

# --- Canary trace inventory ---------------------------------------------------
# The entrypoints the triangle actually calls = the vk* symbols it imports. The real proof: EVERY
# imported symbol must be one the ICD exports (derived from get_proc's RET()/strcmp set in icd.cpp,
# so the check tracks the ICD instead of a hand-kept list), AND the expected draw-spine symbols must
# be present (so a regression that stops driving the draw path is caught). A static guard -- it runs
# even without the boundary up.
imported="$(nm -u "$triangle" 2>/dev/null | grep -oE 'vk[A-Za-z0-9]+' | sort -u || true)"
[ -n "$imported" ] || fail "trace inventory: could not read imported vk* symbols from $triangle"
echo "run_triangle: canary trace inventory ($(echo "$imported" | wc -l) entrypoints):" >&2
echo "$imported" | sed 's/^/  /' >&2

# The ICD's supported set: every RET(Name) in get_proc becomes vkName, plus any vk* returned via an
# explicit strcmp alias (e.g. vkGetPhysicalDeviceProperties2KHR).
icd_src="$here/../icd/icd.cpp"
[ -e "$icd_src" ] || fail "trace inventory: ICD source not found at $icd_src"
supported="$( { grep -oE 'RET\([A-Za-z0-9]+\)' "$icd_src" | sed -E 's/RET\((.+)\)/vk\1/'
               grep -oE '"vk[A-Za-z0-9]+"' "$icd_src" | tr -d '"'; } | sort -u )"
# Every imported symbol must be in the supported set (this subsumes the "no " check: a
# entrypoint is not exported, so it would show up here).
unsupported="$(comm -23 <(echo "$imported") <(echo "$supported"))"
if [ -n "$unsupported" ]; then
    echo "$unsupported" | sed 's/^/  /' >&2
    fail "trace inventory: the triangle imports entrypoints the ICD does not export (above)"
fi
# The draw spine must actually be exercised (guards against silently dropping back to a clear-only
# path): require the full set of draw entrypoints among the imports.
for sym in vkCreateImageView vkCreateShaderModule vkCreateRenderPass vkCreateFramebuffer \
           vkCreatePipelineLayout vkCreateGraphicsPipelines vkCmdBeginRenderPass vkCmdEndRenderPass \
           vkCmdBindPipeline vkCmdSetViewport vkCmdSetScissor vkCmdDraw; do
    echo "$imported" | grep -qx "$sym" || fail "trace inventory: expected draw entrypoint $sym not imported"
done
echo "run_triangle: trace inventory OK (all imports ICD-supported; full draw spine present)" >&2

trap 'vkr_session_cleanup "$?"' EXIT

vkr_start_private_display || fail "private display setup failed"
vkr_open_session_and_pin_icd "$launch" "$manifest" vkrelay2-triangle \
    || fail "session/ICD setup failed (is the daemon up as '--serve --vulkan-backend real'?)"

log="$VKR_RUNTIME_DIR/triangle.log"
echo "run_triangle: running the triangle canary (VK_ICD_FILENAMES=$manifest)" >&2
rc=0
VK_LOADER_DEBUG=driver "$triangle" >"$log" 2>&1 || rc=$?
cat "$log" >&2

# Assert the draw chain. "data plane connected ... app_ack ok" is printed ONLY by our ICD, so it
# proves the loader selected libvulkan_vkrelay2.so + the handshake succeeded; the rest proves the
# worker built the real pipeline and drew + presented the triangle.
assert() { grep -q "$1" "$log" || fail "chain assertion failed: missing '$1' (see $log)"; }
assert "vkrelay2-icd: data plane connected"
assert "app_ack ok"
assert "vkrelay2-triangle: surface created"
assert "vkrelay2-triangle: swapchain created"
assert "vkrelay2-triangle: graphics pipeline created"
assert "vkrelay2-triangle: presented frame"

[ "$rc" -eq 0 ] || fail "triangle canary exited non-zero ($rc) despite chain markers (see $log)"

echo "run_triangle: PASS -- loader selected the vkrelay2 ICD; bufferless triangle drawn + presented via worker-present" >&2
