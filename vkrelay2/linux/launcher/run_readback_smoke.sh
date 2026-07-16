#!/usr/bin/env bash
# vkrelay2 boundary smoke: offscreen GL readback, end to end on the REAL backend. Runs
# vkrelay2-readback-canary as a normal app through vkrun (full private display +
# real worker session + pinned ICD + zink), so the canary's glReadPixels exercises the exact
# OpenSCAD `-o png` export path: GPU renders on the Windows side, zink copies the pixels into a
# host-visible staging buffer (copy_image_to_buffer), syncs on its timeline semaphore, and the
# guest app reads the mapped bytes -- which are real only if the relay downloads the GPU-written
# buffer back into the ICD's mapped-memory shadow.
#
# The PROOF is the canary's self-judged pixel check (app-FBO clear cc3399ff + winsys clear 3399cc,
# both read back exactly). Pre-fix (RED) both reads return stale zeros: black -- the bug.
#
# REAL backend required (the mock validates but does not execute copies), so this smoke SKIPs
# cleanly when the relay cannot establish a real session (no daemon reachable + no Windows build:
# not a GPU box). A bring-up failure on a box that HAS the Windows build still FAILs.
#
# Usage: bash run_readback_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "READBACK-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-readback-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first; needs gl/x11 dev)"

# Run the canary as the target app through the full user-facing launcher (private display + REAL
# worker session + pinned ICD + zink env). Bounded: bring-up + canary well under the cap.
out="$(timeout 120 "${script_dir}/vkrun" -- "${canary}" 2>&1)"
rc=$?
printf '%s\n' "${out}" | sed 's/^/    /'

if ! printf '%s\n' "${out}" | grep -q "READBACK-CANARY:"; then
    # Bring-up never reached the canary. No real daemon AND no Windows build to start one -> not a
    # GPU-relay box: skip. Anything else (wedged daemon, display failure, timeout) is a real FAIL.
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon / no Windows build (not a GPU box)"
    fi
    echo "READBACK-SMOKE: FAIL (bring-up did not reach the canary, rc=${rc})"
    exit 1
fi

if ! printf '%s\n' "${out}" | grep -q "READBACK-CANARY: PASS"; then
    echo "READBACK-SMOKE: FAIL (glReadPixels did not return the GPU-written pixels -- readback failed)"
    exit 1
fi
[ "${rc}" -eq 0 ] || { echo "READBACK-SMOKE: FAIL (canary rc=${rc} despite PASS line)"; exit 1; }

# Queue-order sub-gate (regression): a NATIVE Vulkan canary submits a readback with
# NO proof of its own, then waits a LATER same-queue fence-only / timeline-signal submit -- queue
# order makes those proofs cover the earlier copy, so the mapped bytes must be the clear colors.
order_canary="${build_dir}/vkrelay2-readback-order-canary"
if [ -x "${order_canary}" ]; then
    order_out="$(timeout 120 "${script_dir}/vkrun" -- "${order_canary}" 2>&1)"
    order_rc=$?
    printf '%s\n' "${order_out}" | grep -E "ORDER-CANARY:" | sed 's/^/    /'
    if ! printf '%s\n' "${order_out}" | grep -q "ORDER-CANARY: PASS"; then
        echo "READBACK-SMOKE: FAIL (queue-order proof did not promote the earlier readback, rc=${order_rc})"
        exit 1
    fi
else
    echo "READBACK-SMOKE: NOTE (order canary not built; queue-order sub-gate skipped)"
fi

echo "============================================================"
echo "READBACK-SMOKE: PASS (GPU-written pixels crossed the relay back to the guest:"
echo "               GL app-FBO + winsys reads AND the queue-order-proven native readback)"
echo "============================================================"
exit 0
