#!/usr/bin/env bash
# vkrelay2 vertex-attr-divisor smoke: the real-GPU gate for VK_EXT_vertex_attribute_divisor.
# It brings up the create-only vkrelay2-vertex-divisor canary through vkrun and
# requires a full PASS: a graphics pipeline whose vertex-input state chains
# VkPipelineVertexInputDivisorStateCreateInfoEXT (binding 1 INSTANCE-rate, divisor 2) -- the exact
# pNext the ICD used to reject wholesale (Blender/zink hit it) -- must create on the real GPU.
#
# REAL backend required (a real vkCreateGraphicsPipelines that reaches the host); SKIPs cleanly
# (exit 0) when the relay cannot establish a real session (not a GPU box).
#
# Usage: bash run_vertex_divisor_smoke.sh [<build-dir>]   (default: ../../build/linux-release)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-release}"

skip() { echo "VERTEX-DIVISOR-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vertex-divisor"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

out="$(timeout 120 "${script_dir}/vkrun" -- "${canary}" 2>&1)"
printf '%s\n' "${out}" | grep -E "vkrelay2-vtxdivisor:" | sed 's/^/    /' >&2

if ! printf '%s\n' "${out}" | grep -q "vkrelay2-vtxdivisor:"; then
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon / no Windows build (not a GPU box)"
    fi
    echo "VERTEX-DIVISOR-SMOKE: FAIL (canary produced no output)"
    exit 1
fi
# The canary SKIPs itself (exit 0, "skipped") when no ICD/worker is reachable -- treat as SKIP.
if printf '%s\n' "${out}" | grep -q "vkrelay2-vtxdivisor: skipped"; then
    skip "canary self-skipped (no reachable ICD/worker)"
fi
if printf '%s\n' "${out}" | grep -q "PASS: divisor pipeline created on the real GPU"; then
    echo "VERTEX-DIVISOR-SMOKE: PASS"
    exit 0
fi
echo "VERTEX-DIVISOR-SMOKE: FAIL (no PASS line -- divisor pipeline was rejected?)"
exit 1
