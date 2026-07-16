#!/usr/bin/env bash
# vkrelay2 dynamic-rendering smoke: the steering-safe NATIVE lane + VK_KHR_dynamic_
# rendering, end to end on the REAL backend, AND the steering-intact proof that the default (zink)
# lane never sees it. Three bring-ups of vkrelay2-vk13-dynrender-canary through vkrun:
#   1. NATIVE lane  (--frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1): the canary must PASS -- the
#      device now reports 1.3 on a 1.3-capable host (the required-feature audit),
#      where DR is CORE (still reachable via its KHR alias); a NULL-renderpass pipeline + vkCmdBegin/
#      End RenderingKHR clear+draw controls the offscreen pixels (corner == clear color, center drew).
#   2. DEFAULT lane (no flag -> zink): the canary reports `dynamic_rendering=0` (absent -- the 1.2
#      steering is untouched). The canary's overall FAIL here is EXPECTED (it targets the native
#      lane); we assert the `dynamic_rendering=0` line, the steering proof.
#   3. CONTAMINATED parent env: VKRELAY2_NATIVE_LANE=1 exported in the shell + the DEFAULT frontend.
#      The launcher OVERRIDES the marker to 0 for zink modes, so the canary still reports
#      `dynamic_rendering=0` -- a stray parent env can NEVER uncap a zink run.
#
# REAL backend required (a NULL-renderpass pipeline + dynamic-rendering commands that reach the host);
# SKIPs cleanly when the relay cannot establish a real session (not a GPU box).
#
# Usage: bash run_vk13_dynrender_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-DYNREND-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-dynrender-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay with the given launcher args; echo its VK13-DYNREND-CANARY lines to
# stderr (indented) and to stdout (raw, for grepping). SKIPs the whole smoke if not a GPU box.
run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" | grep -E "VK13-DYNREND-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-DYNREND-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-DYNREND-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# 1. NATIVE lane: full PASS + dynamic_rendering=1.
native="$(run_canary native --frontend vulkan13)"
if ! printf '%s\n' "${native}" | grep -q "VK13-DYNREND-CANARY: PASS"; then
    echo "VK13-DYNREND-SMOKE: FAIL (native lane: the DR draw did not pass -- see the lines above)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -qE "VK13-DYNREND-CANARY: extensions dynamic_rendering=1 "; then
    echo "VK13-DYNREND-SMOKE: FAIL (native lane: VK_KHR_dynamic_rendering not advertised)"
    exit 1
fi

# 2. DEFAULT lane: steering intact -- dynamic_rendering=0. The canary FAILs overall here by design.
default="$(run_canary default)"
if ! printf '%s\n' "${default}" | grep -qE "VK13-DYNREND-CANARY: extensions dynamic_rendering=0 "; then
    echo "VK13-DYNREND-SMOKE: FAIL (default lane leaked dynamic_rendering -- the 1.2 zink steering"
    echo "                     was disturbed)"
    exit 1
fi

# 3. CONTAMINATED parent env + default frontend: the launcher override wins -> still 0.
contam="$(VKRELAY2_NATIVE_LANE=1 run_canary contaminated)"
if ! printf '%s\n' "${contam}" | grep -qE "VK13-DYNREND-CANARY: extensions dynamic_rendering=0 "; then
    echo "VK13-DYNREND-SMOKE: FAIL (a contaminated parent VKRELAY2_NATIVE_LANE=1 uncapped a zink run"
    echo "                     -- the launcher did not neutralize the marker for the default frontend)"
    exit 1
fi

echo "============================================================"
echo "VK13-DYNREND-SMOKE: PASS (native lane serves VK_KHR_dynamic_rendering: a NULL-renderpass"
echo "                   pipeline + vkCmdBegin/EndRenderingKHR clear+draw on the real GPU; the"
echo "                   default/zink lane stays 1.2 with NO dynamic_rendering, even with a"
echo "                   contaminated parent env)"
echo "============================================================"
exit 0
