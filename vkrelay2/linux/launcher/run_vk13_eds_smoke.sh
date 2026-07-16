#!/usr/bin/env bash
# vkrelay2 Vulkan-1.3 opener smoke: the steering-safe NATIVE lane + EDS1, end to
# end on the REAL backend, AND the steering-intact proof that the default (zink) lane never sees it.
# Three bring-ups of vkrelay2-vk13-eds-canary through vkrun:
#   1. NATIVE lane  (--frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1): the canary must PASS -- the
#      device now reports 1.3 on a 1.3-capable host (the required-feature audit),
#      where EDS is CORE (still reachable via its EXT alias); a dynamic vkCmdSetCullModeEXT controls
#      the offscreen draw (center pixel colored with cull NONE, clear with cull FRONT_AND_BACK).
#   2. DEFAULT lane (no flag -> zink): the canary reports `eds=0` (EDS absent -- the 1.2 steering is
#      untouched). The canary's overall FAIL here is EXPECTED (it targets the native lane); we assert
#      the `eds=0` line, the steering proof.
#   3. CONTAMINATED parent env: VKRELAY2_NATIVE_LANE=1 exported in the shell + the DEFAULT frontend.
#      The launcher OVERRIDES the marker to 0 for zink modes, so the canary still reports `eds=0` --
#      a stray parent env can NEVER uncap a zink run (the "unbreakable by construction" claim).
#
# REAL backend required (a dynamic cull that reaches the host rasterizer); SKIPs cleanly when the
# relay cannot establish a real session (not a GPU box).
#
# Usage: bash run_vk13_eds_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-EDS-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-eds-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay with the given launcher args; echo its VK13-EDS-CANARY lines to
# stderr (indented) and to stdout (raw, for grepping). SKIPs the whole smoke if not a GPU box.
run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" | grep -E "VK13-EDS-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-EDS-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-EDS-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# 1. NATIVE lane: full PASS + eds=1.
native="$(run_canary native --frontend vulkan13)"
if ! printf '%s\n' "${native}" | grep -q "VK13-EDS-CANARY: PASS"; then
    echo "VK13-EDS-SMOKE: FAIL (native lane: the EDS draw did not pass -- see the lines above)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -qE "VK13-EDS-CANARY: extensions eds=1 "; then
    echo "VK13-EDS-SMOKE: FAIL (native lane: VK_EXT_extended_dynamic_state not advertised)"
    exit 1
fi

# 2. DEFAULT lane: steering intact -- eds=0 (EDS absent). The canary FAILs overall here by design.
default="$(run_canary default)"
if ! printf '%s\n' "${default}" | grep -qE "VK13-EDS-CANARY: extensions eds=0 "; then
    echo "VK13-EDS-SMOKE: FAIL (default lane leaked EDS -- the 1.2 zink steering was disturbed)"
    exit 1
fi

# 3. CONTAMINATED parent env + default frontend: the launcher override wins -> still eds=0.
contam="$(VKRELAY2_NATIVE_LANE=1 run_canary contaminated)"
if ! printf '%s\n' "${contam}" | grep -qE "VK13-EDS-CANARY: extensions eds=0 "; then
    echo "VK13-EDS-SMOKE: FAIL (a contaminated parent VKRELAY2_NATIVE_LANE=1 uncapped a zink run --"
    echo "                 the launcher did not neutralize the marker for the default frontend)"
    exit 1
fi

echo "============================================================"
echo "VK13-EDS-SMOKE: PASS (native lane exposes VK_EXT_extended_dynamic_state + a dynamic"
echo "               vkCmdSetCullModeEXT controls rasterization on the real GPU; the default/zink"
echo "               lane stays 1.2 with NO EDS, even with a contaminated parent env)"
echo "============================================================"
exit 0
