#!/usr/bin/env bash
# vkrelay2 multiview SERVE-PROOF smoke (required-feature audit): the release gate
# behind flipping kRelayServesMultiview. It brings up vkrelay2-vk13-multiview-canary through
# vkrun on the NATIVE lane (--frontend vulkan13; the canary also drives a viewMask
# DYNAMIC-rendering scope, which is native-lane-only) and requires a full PASS:
#   - the multiview feature is reported TRUE (required since 1.1);
#   - a gl_ViewIndex-colored full-screen triangle rendered into a 2-layer image reads back layer 0 =
#     RED and layer 1 = GREEN -- through BOTH a vkCreateRenderPass2 viewMask pass AND a
#     vkCmdBeginRenderingKHR viewMask scope. If multiview did not run per-view on the host
#     both layers would carry one color, so this distinguishes a real serve from a silent fallback.
#
# REAL backend required (a real multiview render pass + per-layer readback that reach the host);
# SKIPs cleanly (exit 0) when the relay cannot establish a real session (not a GPU box).
#
# Usage: bash run_vk13_multiview_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-MULTIVIEW-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-multiview-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" |
        grep -E "build: (linux launch|windows worker) flavor=|WARNING.*(stale|OLDER)" |
        sed "s/^/    [${label}] /" >&2
    printf '%s\n' "${out}" | grep -E "VK13-MULTIVIEW-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-MULTIVIEW-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-MULTIVIEW-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

native="$(run_canary native --frontend vulkan13)"
if ! printf '%s\n' "${native}" | grep -qE "VK13-MULTIVIEW-CANARY: multiview feature=1"; then
    echo "VK13-MULTIVIEW-SMOKE: FAIL (native lane: multiview reported FALSE -- required since 1.1)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -q "VK13-MULTIVIEW-CANARY: PASS"; then
    echo "VK13-MULTIVIEW-SMOKE: FAIL (native lane: the multiview serve-proof did not pass -- per-view"
    echo "                     gl_ViewIndex color did not land on the right layers; see the lines above)"
    exit 1
fi

echo "============================================================"
echo "VK13-MULTIVIEW-SMOKE: PASS (multiview is served end-to-end on the real GPU: a gl_ViewIndex"
echo "                     render lands red on layer 0 + green on layer 1 through BOTH the"
echo "                     render-pass2 viewMask path AND the dynamic-rendering viewMask path)"
echo "============================================================"
exit 0
