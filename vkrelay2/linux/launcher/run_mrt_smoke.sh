#!/usr/bin/env bash
# vkrelay2 MRT boundary smoke: multiple color attachments, end to end on the REAL backend. Runs
# vkrelay2-mrt-canary as a normal app through vkrun (full private display + real
# worker session + pinned ICD + zink): a fullscreen triangle writes DISTINCT colors to multiple
# FBO attachments, each read back exactly. Three sub-gates ride one canary:
#   A. 2-attachment distinct colors -- the development probe shape that SILENTLY half-rendered
#      before MRT support (FBO COMPLETE, att0 correct, att1 black);
#   B. a per-attachment write mask (glColorMaski) -- the faithful blend-state ARRAY applies per
#      attachment;
#   C. gapped draw buffers {ATT0, GL_NONE, ATT2} -- a VK_ATTACHMENT_UNUSED color-ref hole on the
#      wire; the gap attachment keeps its clear color.
#
# REAL backend required; SKIPs cleanly when the relay cannot establish a real session (no daemon
# reachable + no Windows build: not a GPU box). A bring-up failure on a GPU box still FAILs.
#
# Usage: bash run_mrt_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "MRT-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-mrt-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first; needs gl/x11 dev)"

out="$(timeout 120 "${script_dir}/vkrun" -- "${canary}" 2>&1)"
rc=$?
printf '%s\n' "${out}" | grep -E "MRT-CANARY:" | sed 's/^/    /'

if ! printf '%s\n' "${out}" | grep -q "MRT-CANARY:"; then
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon / no Windows build (not a GPU box)"
    fi
    echo "MRT-SMOKE: FAIL (bring-up did not reach the canary, rc=${rc})"
    exit 1
fi

if ! printf '%s\n' "${out}" | grep -q "MRT-CANARY: PASS"; then
    echo "MRT-SMOKE: FAIL (an MRT sub-gate rendered wrong -- see the canary lines above)"
    exit 1
fi
[ "${rc}" -eq 0 ] || { echo "MRT-SMOKE: FAIL (canary rc=${rc} despite PASS line)"; exit 1; }

echo "============================================================"
echo "MRT-SMOKE: PASS (distinct colors on multiple attachments, a per-attachment write"
echo "           mask, and an UNUSED-gap draw-buffer set all render exactly through the relay)"
echo "============================================================"
exit 0
