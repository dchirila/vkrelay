#!/usr/bin/env bash
# vkrelay2 Vulkan 1.3 support smoke: the honest apiVersion-1.3 device on the NATIVE lane, end to
# end on the REAL backend, AND the steering-intact proof that the default (zink) lane still sees
# a 1.2 device. Three bring-ups of vkrelay2-vk13-finale-canary through vkrun:
#   1. NATIVE lane  (--frontend vulkan13): the canary must PASS -- device apiVersion 1.3, every
#      1.3-required feature TRUE with the unserved members FALSE + rejected, tool properties /
#      private data / maintenance4 / cache control / subgroup size answered, an
#      inline-uniform-block dispatch, the core-name dynamic-state surface, featureless-core
#      dynamic rendering + synchronization2, copy_commands2, and vkCmdResolveImage2, all
#      byte-exact.
#   2. DEFAULT lane (no flag -> zink): the canary reports `apiVersion=1.2` (the 1.2 steering that
#      keeps zink off the 1.3 command surfaces is untouched). Its overall FAIL here is EXPECTED.
#   3. CONTAMINATED parent env: VKRELAY2_NATIVE_LANE=1 exported + the DEFAULT frontend; the
#      launcher neutralizes the marker, so still `apiVersion=1.2`.
#
# REAL backend required; SKIPs cleanly when the relay cannot establish a real session.
#
# Usage: bash run_vk13_finale_smoke.sh [<build-dir>]   (default: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-FINALE-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-finale-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" | grep -E "VK13-FINALE-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-FINALE-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-FINALE-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# 1. NATIVE lane: full PASS + apiVersion 1.3 (everything else asserted by the canary itself).
native="$(run_canary native --frontend vulkan13)"
if ! printf '%s\n' "${native}" | grep -q "VK13-FINALE-CANARY: PASS"; then
    echo "VK13-FINALE-SMOKE: FAIL (native lane: the Vulkan 1.3 canary did not pass -- see above)"
    exit 1
fi
if ! printf '%s\n' "${native}" | grep -qE "VK13-FINALE-CANARY: device .* apiVersion=1\.3"; then
    echo "VK13-FINALE-SMOKE: FAIL (native lane: device not reported as apiVersion 1.3)"
    exit 1
fi

# 2. DEFAULT lane: steering intact -- still a 1.2 device, AND the core-1.3-only names + the
#    non-enabled extension aliases do not even RESOLVE on it (the honest proc-address surface).
#    The canary FAILs overall by design.
default="$(run_canary default)"
if ! printf '%s\n' "${default}" | grep -qE "VK13-FINALE-CANARY: device .* apiVersion=1\.2"; then
    echo "VK13-FINALE-SMOKE: FAIL (default lane no longer reports a 1.2 device -- the zink"
    echo "                  steering was disturbed)"
    exit 1
fi
if ! printf '%s\n' "${default}" | grep -q "VK13-FINALE-CANARY: proc_gate_on_12_device=1"; then
    echo "VK13-FINALE-SMOKE: FAIL (a core-1.3 / non-enabled-extension name RESOLVED on the 1.2"
    echo "                  default lane -- the device-aware proc gate was disturbed)"
    exit 1
fi

# 3. CONTAMINATED parent env + default frontend: the launcher override wins -> still 1.2.
contam="$(VKRELAY2_NATIVE_LANE=1 run_canary contaminated)"
if ! printf '%s\n' "${contam}" | grep -qE "VK13-FINALE-CANARY: device .* apiVersion=1\.2"; then
    echo "VK13-FINALE-SMOKE: FAIL (a contaminated parent VKRELAY2_NATIVE_LANE=1 uncapped a zink"
    echo "                  run)"
    exit 1
fi

echo "============================================================"
echo "VK13-FINALE-SMOKE: PASS (the native lane serves an honest apiVersion-1.3 device -- required"
echo "                  features, private data, maintenance4, cache control, subgroup size,"
echo "                  inline uniform blocks, the core dynamic-state surface, featureless-core"
echo "                  dynamic rendering + synchronization2, copy_commands2, and resolve2 --"
echo "                  while the default/zink lane stays a 1.2 device, even with a contaminated"
echo "                  parent env)"
echo "============================================================"
exit 0
