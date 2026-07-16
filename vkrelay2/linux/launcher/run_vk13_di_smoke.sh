#!/usr/bin/env bash
# vkrelay2 descriptorIndexing smoke (buffer-only bindless support): the steering-safe
# NATIVE lane + the served-subset contract, end to end on the REAL backend, AND the
# steering-intact proof that the default (zink) lane never sees it. Three bring-ups of
# vkrelay2-vk13-di-canary through vkrun:
#   1. NATIVE lane  (--frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1): the canary must PASS -- device
#      still 1.2, the 8 served members TRUE (rollup + standalone agreeing) with the AGGREGATE and
#      every unserved member FALSE, aggregate/deferred enables FEATURE_NOT_PRESENT, the host's
#      layout-support answer through RpcOp 88, and the record -> update-after-bind (destroy-old /
#      repoint) -> submit crux byte-exact on the real GPU with partially-bound slots unwritten and
#      the variable count below its declared max.
#   2. DEFAULT lane (no flag -> zink): the canary reports `di_served=0` (masked -- the 1.2 steering
#      that keeps zink off bindless is untouched) and `di_unserved_leak=0`. The canary's overall
#      FAIL here is EXPECTED (it targets the native lane); we assert the mask lines.
#   3. CONTAMINATED parent env: VKRELAY2_NATIVE_LANE=1 exported in the shell + the DEFAULT
#      frontend. The launcher OVERRIDES the marker to 0 for zink modes, so the canary still
#      reports `di_served=0` -- a stray parent env can NEVER uncap a zink run.
#
# REAL backend required (the crux only means anything on the real GPU); SKIPs cleanly when the
# relay cannot establish a real session (not a GPU box).
#
# Usage: bash run_vk13_di_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-DI-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-di-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay with the given launcher args; echo its VK13-DI-CANARY lines to
# stderr (indented) and stdout (raw, for grepping). SKIPs the whole smoke if not a GPU box.
run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" | grep -E "VK13-DI-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-DI-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-DI-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# 1. NATIVE lane: full PASS + di_served=1 (the aggregate/deferred mask and the negative creates
#    are asserted by the canary itself, which FAILs on any leak).
native="$(run_canary native --frontend vulkan13)"
if ! printf '%s\n' "${native}" | grep -q "VK13-DI-CANARY: PASS"; then
    echo "VK13-DI-SMOKE: FAIL (native lane: descriptorIndexing did not pass -- see the lines"
    echo "               above)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -qE "VK13-DI-CANARY: feature di_served=1 di_unserved_leak=0"; then
    echo "VK13-DI-SMOKE: FAIL (native lane: the served subset was not reported TRUE, or an"
    echo "               unserved member leaked)"
    exit 1
fi

# 2. DEFAULT lane: steering intact -- di_served=0 and no unserved leak. The canary FAILs overall
#    here by design (it targets the native lane).
default="$(run_canary default)"
if ! printf '%s\n' "${default}" | grep -qE "VK13-DI-CANARY: feature di_served=0 di_unserved_leak=0"; then
    echo "VK13-DI-SMOKE: FAIL (default lane leaked a descriptorIndexing bit -- the 1.2 zink"
    echo "               steering was disturbed)"
    exit 1
fi

# 3. CONTAMINATED parent env + default frontend: the launcher override wins -> still masked.
contam="$(VKRELAY2_NATIVE_LANE=1 run_canary contaminated)"
if ! printf '%s\n' "${contam}" | grep -qE "VK13-DI-CANARY: feature di_served=0 di_unserved_leak=0"; then
    echo "VK13-DI-SMOKE: FAIL (a contaminated parent VKRELAY2_NATIVE_LANE=1 uncapped a zink run"
    echo "               -- the launcher did not neutralize the marker for the default frontend)"
    exit 1
fi

echo "============================================================"
echo "VK13-DI-SMOKE: PASS (native lane serves the buffer-only descriptorIndexing subset: served"
echo "               bits TRUE with the aggregate + every unserved member FALSE, deferred enables"
echo "               reject, the host answers DI layout support, and the record ->"
echo "               update-after-bind (destroy-old/repoint) -> submit crux is byte-exact on the"
echo "               real GPU; the default/zink lane stays masked, even with a contaminated"
echo "               parent env)"
echo "============================================================"
exit 0
