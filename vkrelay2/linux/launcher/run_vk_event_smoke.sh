#!/usr/bin/env bash
# vkrelay2 core-1.0 synchronization smoke: vkGetFenceStatus + the VkEvent object
# model + the sync1 command events, end to end on the REAL backend. advertises NOTHING
# (events + vkGetFenceStatus are core Vulkan 1.0), so -- unlike the vk13 smokes -- there is no
# native-lane / steering probe: a SINGLE bring-up of vkrelay2-vk-event-canary on the DEFAULT lane
# must PASS. The canary proves:
#   - vkGetFenceStatus: signaled-create -> VK_SUCCESS, unsignaled -> VK_NOT_READY, submit signals,
#     reset clears;
#   - the host event round-trip (create -> RESET, set -> SET, reset -> RESET);
#   - a submitted vkCmdSetEvent reaching the host (device set -> host status SET);
#   - a vkCmdWaitEvents-guarded clear/copy: a host-set event gates an image UNDEFINED->TRANSFER_DST
#     transition (inside the wait's image barrier) + clear + copy, and the readback equals the clear
#     color.
#
# REAL backend required (events/fences that reach the host); SKIPs cleanly when the relay cannot
# establish a real session (not a GPU box).
#
# Usage: bash run_vk_event_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK-EVENT-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk-event-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay on the DEFAULT lane (events are core 1.0 -- no --frontend). Echo
# its VK-EVENT-CANARY lines to stderr (indented) and stdout (raw, for grepping). SKIPs the whole
# smoke if not a GPU box.
out="$(timeout 120 "${script_dir}/vkrun" -- "${canary}" 2>&1)"
printf '%s\n' "${out}" | grep -E "VK-EVENT-CANARY:" | sed "s/^/    /" >&2
if ! printf '%s\n' "${out}" | grep -q "VK-EVENT-CANARY:"; then
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon / no Windows build (not a GPU box)"
    fi
    echo "VK-EVENT-SMOKE: FAIL (bring-up did not reach the canary)"
    exit 1
fi

# A clean SKIP from the canary itself (no ICD/worker) is not a failure.
if printf '%s\n' "${out}" | grep -q "VK-EVENT-CANARY: SKIP"; then
    skip "canary could not reach an ICD/worker (not a GPU box)"
fi

if ! printf '%s\n' "${out}" | grep -q "VK-EVENT-CANARY: PASS"; then
    echo "VK-EVENT-SMOKE: FAIL (the event/fence-status canary did not pass -- see the lines above)"
    exit 1
fi

echo "============================================================"
echo "VK-EVENT-SMOKE: PASS (vkGetFenceStatus + the VkEvent object model + host/device event set +"
echo "                a vkCmdWaitEvents-guarded transfer all serve on the real GPU)"
echo "============================================================"
exit 0
