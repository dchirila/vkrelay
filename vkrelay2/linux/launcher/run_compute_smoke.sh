#!/usr/bin/env bash
# vkrelay2 compute boundary smoke: the whole compute class, end to end on the REAL
# backend. Runs vkrelay2-compute-canary as a normal app through vkrun (real
# worker session + pinned ICD): compute pipeline create (queue-flags honesty gate) -> storage/
# uniform descriptors + push constants -> bind at the COMPUTE point -> dispatch -> the
# shader-write -> transfer-read BUFFER barrier -> vkCmdCopyBuffer into a mapped readback buffer
# (the buffer-dst readback class covered here) -> fence -> 4096 u32 results asserted
# BYTE-EXACTLY (out[i] == in[i]*3 + 7 + i). Before the fix (RED) the canary dies on the named
# vkCreateComputePipelines abort-stub.
#
# REAL backend required (the mock validates structurally but does not execute shaders), so this
# SKIPs cleanly when the relay cannot establish a real session (not a GPU box). A bring-up
# failure on a GPU box still FAILs.
#
# Usage: bash run_compute_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "COMPUTE-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-compute-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

out="$(timeout 120 "${script_dir}/vkrun" -- "${canary}" 2>&1)"
rc=$?
printf '%s\n' "${out}" | grep -E "COMPUTE-CANARY:" | sed 's/^/    /'

if ! printf '%s\n' "${out}" | grep -q "COMPUTE-CANARY:"; then
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon / no Windows build (not a GPU box)"
    fi
    echo "COMPUTE-SMOKE: FAIL (bring-up did not reach the canary, rc=${rc})"
    exit 1
fi

if ! printf '%s\n' "${out}" | grep -q "COMPUTE-CANARY: PASS"; then
    echo "COMPUTE-SMOKE: FAIL (the compute chain broke -- see the canary lines above)"
    exit 1
fi
[ "${rc}" -eq 0 ] || { echo "COMPUTE-SMOKE: FAIL (canary rc=${rc} despite PASS line)"; exit 1; }

echo "============================================================"
echo "COMPUTE-SMOKE: PASS (compute pipeline + descriptors + push constants + dispatch +"
echo "              buffer barrier + copy-out readback -- 4096 results byte-exact through"
echo "              the relay on the real GPU)"
echo "============================================================"
exit 0
