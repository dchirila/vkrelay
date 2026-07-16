#!/usr/bin/env bash
# AMD-iGPU viewport-corruption diagnostic smoke. Runs the parameterized GL/zink multi-frame canary
# through the normal real-worker launcher on one selected adapter and requires every rung/frame to
# self-judge PASS.
#
# Usage: run_frame_transition_smoke.sh [<build-dir>] [<gpu-selector>]
# Defaults: ../../build/linux-release, integrated
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-release}"
gpu="${2:-integrated}"
canary="${build_dir}/vkrelay2-frame-transition-canary"
launcher="${script_dir}/vkrun"

if [[ ! -x "${canary}" ]]; then
    echo "FRAME-TRANSITION-SMOKE: SKIP (missing ${canary})"
    exit 0
fi

log="$(mktemp /tmp/vkrelay2-frame-transition.XXXXXX.log)"
trap 'rm -f "${log}"' EXIT

rc=0
timeout 120 "${launcher}" --gpu "${gpu}" -- "${canary}" --rung all >"${log}" 2>&1 || rc=$?
grep -E 'FRAME-TRANSITION-CANARY:' "${log}" | sed 's/^/    /' || true

if [[ "${rc}" -ne 0 ]] || ! grep -q '^FRAME-TRANSITION-CANARY: PASS$' "${log}"; then
    echo "FRAME-TRANSITION-SMOKE: FAIL (gpu=${gpu}, rc=${rc}, full log=${log})"
    trap - EXIT
    exit 1
fi

renderer="$(sed -n 's/^FRAME-TRANSITION-CANARY: GL_RENDERER=//p' "${log}" | head -1)"
if [[ -z "${renderer}" || "${renderer,,}" != *zink* ]]; then
    echo "FRAME-TRANSITION-SMOKE: FAIL (gpu=${gpu}, renderer is not zink: ${renderer:-missing})"
    exit 1
fi

for rung in clear-reuse scissor-clear attachment-rotate vbo-update persistent-map; do
    grep -q "^FRAME-TRANSITION-CANARY: rung=${rung} PASS$" "${log}" || {
        echo "FRAME-TRANSITION-SMOKE: FAIL (gpu=${gpu}, rung ${rung} did not pass)"
        exit 1
    }
done

echo "FRAME-TRANSITION-SMOKE: PASS (gpu=${gpu}, renderer=${renderer}; all 16-frame rungs passed)"
