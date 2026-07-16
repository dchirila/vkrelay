#!/usr/bin/env bash
# vkrelay2 tessellation + geometry boundary smoke (breadth): the advertised-TRUE-but-
# untested stages, end to end on the REAL backend. Runs vkrelay2-tessgeom-canary as a normal app
# through vkrun (real worker session + pinned ICD + zink): a GL 4.0 core context
# whose PIXELS prove the stages ran -- the TES shrinks a fullscreen patch to a centered triangle
# (center = tess color, corner = background) and the GS expands one point into a centered quad
# (two far-apart probes + a clean corner) -- both read back exactly.
#
# HONESTY GATE: the canary must report a zink renderer (GL->Vulkan over OUR ICD); a software
# fallback would render the right pixels without touching the relay, so it is a FAIL here.
#
# REAL backend required; SKIPs cleanly when the relay cannot establish a real session.
#
# Usage: bash run_tessgeom_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "TESSGEOM-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-tessgeom-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first; needs gl/x11 dev)"

out="$(timeout 120 "${script_dir}/vkrun" -- "${canary}" 2>&1)"
rc=$?
printf '%s\n' "${out}" | grep -E "TESSGEOM-CANARY:" | sed 's/^/    /'

if ! printf '%s\n' "${out}" | grep -q "TESSGEOM-CANARY:"; then
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon / no Windows build (not a GPU box)"
    fi
    echo "TESSGEOM-SMOKE: FAIL (bring-up did not reach the canary, rc=${rc})"
    exit 1
fi

if ! printf '%s\n' "${out}" | grep -q "TESSGEOM-CANARY: GL_RENDERER=zink"; then
    echo "TESSGEOM-SMOKE: FAIL (renderer is not zink -- the pixels did not cross the relay)"
    exit 1
fi
if ! printf '%s\n' "${out}" | grep -q "TESSGEOM-CANARY: PASS"; then
    echo "TESSGEOM-SMOKE: FAIL (a tess/geom sub-gate rendered wrong -- see the lines above)"
    exit 1
fi
[ "${rc}" -eq 0 ] || { echo "TESSGEOM-SMOKE: FAIL (canary rc=${rc} despite PASS line)"; exit 1; }

echo "============================================================"
echo "TESSGEOM-SMOKE: PASS (tessellation shrink + geometry expansion rendered exactly through"
echo "               the relay on the real GPU, zink renderer confirmed)"
echo "============================================================"
exit 0
