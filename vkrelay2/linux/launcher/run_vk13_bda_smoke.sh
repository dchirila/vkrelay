#!/usr/bin/env bash
# vkrelay2 bufferDeviceAddress smoke (Vulkan 1.2 BDA support): the steering-safe NATIVE lane +
# the raw-GPU-pointer contract, end to end on the REAL backend, AND the steering-intact proof that
# the default (zink) lane never sees it. Three bring-ups of vkrelay2-vk13-bda-canary through
# vkrun:
#   1. NATIVE lane  (--frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1): the canary must PASS -- device
#      still 1.2, bufferDeviceAddress TRUE (rollup + standalone) with captureReplay/multiDevice
#      FALSE, a DEVICE_ADDRESS allocation binds a SHADER_DEVICE_ADDRESS buffer,
#      vkGetBufferDeviceAddress returns non-zero, and a descriptorless buffer-reference compute
#      shader reads AND writes through the raw address on the real GPU (byte-exact readback).
#   2. DEFAULT lane (no flag -> zink): the canary reports `bufferDeviceAddress=0` (masked -- the
#      1.2 steering that keeps zink off bindless/BDA is untouched). The canary's overall FAIL here
#      is EXPECTED (it targets the native lane); we assert the `bufferDeviceAddress=0` line.
#   3. CONTAMINATED parent env: VKRELAY2_NATIVE_LANE=1 exported in the shell + the DEFAULT
#      frontend. The launcher OVERRIDES the marker to 0 for zink modes, so the canary still
#      reports `bufferDeviceAddress=0` -- a stray parent env can NEVER uncap a zink run.
#
# REAL backend required (a device address only means anything on the real GPU); SKIPs cleanly when
# the relay cannot establish a real session (not a GPU box).
#
# Usage: bash run_vk13_bda_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-BDA-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-bda-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay with the given launcher args; echo its VK13-BDA-CANARY lines to
# stderr (indented) and stdout (raw, for grepping). SKIPs the whole smoke if not a GPU box.
run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" | grep -E "VK13-BDA-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-BDA-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-BDA-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# 1. NATIVE lane: full PASS + bufferDeviceAddress=1 (and the unwired bits stay 0 -- asserted by
#    the canary itself, which FAILs on a captureReplay/multiDevice leak).
native="$(run_canary native --frontend vulkan13)"
if ! printf '%s\n' "${native}" | grep -q "VK13-BDA-CANARY: PASS"; then
    echo "VK13-BDA-SMOKE: FAIL (native lane: bufferDeviceAddress did not pass -- see the lines"
    echo "                above)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -qE "VK13-BDA-CANARY: feature bufferDeviceAddress=1"; then
    echo "VK13-BDA-SMOKE: FAIL (native lane: bufferDeviceAddress not reported TRUE)"
    exit 1
fi

# 2. DEFAULT lane: steering intact -- bufferDeviceAddress=0. The canary FAILs overall here by
#    design (it targets the native lane).
default="$(run_canary default)"
if ! printf '%s\n' "${default}" | grep -qE "VK13-BDA-CANARY: feature bufferDeviceAddress=0"; then
    echo "VK13-BDA-SMOKE: FAIL (default lane leaked bufferDeviceAddress -- the 1.2 zink steering"
    echo "                was disturbed)"
    exit 1
fi

# 3. CONTAMINATED parent env + default frontend: the launcher override wins -> still 0.
contam="$(VKRELAY2_NATIVE_LANE=1 run_canary contaminated)"
if ! printf '%s\n' "${contam}" | grep -qE "VK13-BDA-CANARY: feature bufferDeviceAddress=0"; then
    echo "VK13-BDA-SMOKE: FAIL (a contaminated parent VKRELAY2_NATIVE_LANE=1 uncapped a zink run"
    echo "                -- the launcher did not neutralize the marker for the default frontend)"
    exit 1
fi

echo "============================================================"
echo "VK13-BDA-SMOKE: PASS (native lane serves bufferDeviceAddress: DEVICE_ADDRESS allocation +"
echo "                SHADER_DEVICE_ADDRESS bind + a non-zero vkGetBufferDeviceAddress + a"
echo "                descriptorless buffer-reference compute shader through the raw address on"
echo "                the real GPU; the default/zink lane stays masked, even with a contaminated"
echo "                parent env)"
echo "============================================================"
exit 0
