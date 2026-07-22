#!/usr/bin/env bash
# vkrelay2 synchronization2 smoke: the steering-safe NATIVE lane + VK_KHR_
# synchronization2, end to end on the REAL backend, AND the steering-intact proof that the default
# (zink) lane remains a 1.2 device. Three bring-ups of vkrelay2-vk13-sync2-canary through
# vkrun:
#   1. NATIVE lane  (--frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1): the canary must PASS -- the
#      device now reports 1.3 on a 1.3-capable host (the required-feature audit),
#      where sync2 is CORE (still reachable via its KHR alias) + feature TRUE + rollup
#      synchronization2=1, and a compute read-after-write ACROSS vkCmdPipelineBarrier2 +
#      vkCmdWriteTimestamp2 + vkCmdSetEvent2 + vkQueueSubmit2 all control the offscreen bytes.
#   2. DEFAULT lane (no flag -> zink): the canary reports apiVersion 1.2 and dynamicRendering=0,
#      while synchronization2 remains available as the KHR extension required by Mesa's classic
#      zero-stage barriers. This is the current steering contract.
#   3. CONTAMINATED parent env: VKRELAY2_NATIVE_LANE=1 exported in the shell + the DEFAULT frontend.
#      The launcher OVERRIDES the marker to 0 for zink modes, so the canary still reports 1.2 with
#      dynamicRendering=0 -- a stray parent env can NEVER uncap a zink run.
#
# REAL backend required (sync2 commands that reach the host); SKIPs cleanly when the relay cannot
# establish a real session (not a GPU box).
#
# Usage: bash run_vk13_sync2_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-SYNC2-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-sync2-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay with the given launcher args; echo its VK13-SYNC2-CANARY lines to
# stderr (indented) and stdout (raw, for grepping). SKIPs the whole smoke if not a GPU box.
run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" | grep -E "VK13-SYNC2-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-SYNC2-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-SYNC2-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

assert_lifetime_canaries() { # <label> <captured output>
    local label="$1"
    local out="$2"
    if ! printf '%s\n' "${out}" | grep -q "recording lifetime lease preserved" ||
       ! printf '%s\n' "${out}" | grep -q "sync2 image lifetime lease survived destroy before End";
    then
        echo "VK13-SYNC2-SMOKE: FAIL (${label}: lifetime-lease canaries did not both pass)"
        exit 1
    fi
}

# 1. NATIVE lane: full PASS + synchronization2=1.
native="$(run_canary native --frontend vulkan13)"
assert_lifetime_canaries native "${native}"
if ! printf '%s\n' "${native}" | grep -q "VK13-SYNC2-CANARY: PASS"; then
    echo "VK13-SYNC2-SMOKE: FAIL (native lane: the sync2 family did not pass -- see the lines above)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -qE "VK13-SYNC2-CANARY: extensions synchronization2=1"; then
    echo "VK13-SYNC2-SMOKE: FAIL (native lane: VK_KHR_synchronization2 not advertised)"
    exit 1
fi

# 2. DEFAULT lane: synchronization2 is deliberately served as an extension, but the device stays
# 1.2 and dynamic rendering stays hidden.
default="$(run_canary default)"
assert_lifetime_canaries default "${default}"
if ! printf '%s\n' "${default}" | grep -qE "VK13-SYNC2-CANARY: device .* apiVersion=1\.2" ||
   ! printf '%s\n' "${default}" | grep -qE "rollup synchronization2=1 dynamicRendering=0"; then
    echo "VK13-SYNC2-SMOKE: FAIL (default lane did not retain the 1.2 / no-dynamic-rendering"
    echo "                    steering contract)"
    exit 1
fi

# 3. CONTAMINATED parent env + default frontend: the launcher override wins -> same 1.2 contract.
contam="$(VKRELAY2_NATIVE_LANE=1 run_canary contaminated)"
assert_lifetime_canaries contaminated "${contam}"
if ! printf '%s\n' "${contam}" | grep -qE "VK13-SYNC2-CANARY: device .* apiVersion=1\.2" ||
   ! printf '%s\n' "${contam}" | grep -qE "rollup synchronization2=1 dynamicRendering=0"; then
    echo "VK13-SYNC2-SMOKE: FAIL (a contaminated parent VKRELAY2_NATIVE_LANE=1 uncapped the"
    echo "                    default frontend)"
    exit 1
fi

echo "============================================================"
echo "VK13-SYNC2-SMOKE: PASS (native lane serves VK_KHR_synchronization2: a compute RAW across"
echo "                  vkCmdPipelineBarrier2 + WriteTimestamp2 + SetEvent2 + QueueSubmit2 on the"
echo "                  real GPU; the default/zink lane stays 1.2 without dynamic rendering, even"
echo "                  with a contaminated parent env; both lanes pass lifetime-lease canaries)"
echo "============================================================"
exit 0
