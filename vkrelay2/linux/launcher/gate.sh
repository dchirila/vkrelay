#!/usr/bin/env bash
# vkrelay2 opt-in end-to-end smoke gate.
#
# Composes existing self-judging feature/WM runners with the real-application render catalog. It
# intentionally does not duplicate their setup or assertions.
#
# Usage: bash gate.sh [<build-dir>] [--strict] [--keep-going]
#   --strict       require every external render-catalog application
#   --keep-going   run every stage and report all failures (default: fail fast)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${script_dir}/../../build/linux-debug"
strict=0
keep_going=0

if [ "${1:-}" != "" ] && [ "${1#--}" = "${1}" ]; then
    build_dir="$1"
    shift
fi
while [ "$#" -gt 0 ]; do
    case "$1" in
        --strict) strict=1 ;;
        --keep-going) keep_going=1 ;;
        -h | --help)
            sed -n '2,/^set -u$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "SMOKE-GATE: FAIL (unknown option '$1')"
            exit 2
            ;;
    esac
    shift
done

artifact_root="${VKRELAY2_SMOKE_GATE_DIR:-${TMPDIR:-/tmp}/vkrelay2-smoke-gate-$$}"
mkdir -p "${artifact_root}"
failures=0
passes=0

run_stage_with_budget() { # <name> <seconds> <command...>
    local name="$1" budget="$2"
    shift 2
    local log="${artifact_root}/${name}.log"
    local rc
    echo
    echo "SMOKE-GATE: >>> ${name}"
    set +e
    timeout --signal=TERM --kill-after=10 "${budget}" "$@" 2>&1 | tee "${log}"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "${rc}" -eq 0 ]; then
        echo "SMOKE-GATE: <<< ${name}: PASS"
        passes=$((passes + 1))
        return 0
    fi
    echo "SMOKE-GATE: <<< ${name}: FAIL (rc=${rc}; log=${log})"
    failures=$((failures + 1))
    if [ "${keep_going}" -eq 0 ]; then
        echo "SMOKE-GATE: FAIL (fail-fast; artifacts preserved in ${artifact_root})"
        exit 1
    fi
    return 0
}

run_stage() { # <name> <command...>
    local name="$1"
    shift
    run_stage_with_budget "${name}" "${VKRELAY2_SMOKE_STAGE_TIMEOUT:-360}" "$@"
}

# Turn on errexit only after run_stage exists; each stage temporarily disables it to collect rc.
set -e

run_stage readback bash "${script_dir}/run_readback_smoke.sh" "${build_dir}"
run_stage tessgeom bash "${script_dir}/run_tessgeom_smoke.sh" "${build_dir}"
run_stage mrt bash "${script_dir}/run_mrt_smoke.sh" "${build_dir}"
run_stage frame-transition bash "${script_dir}/run_frame_transition_smoke.sh" \
    "${build_dir}" "${VKRELAY2_SMOKE_GPU:-integrated}"
run_stage real-gl-apps bash "${script_dir}/run_gl_apps_smoke.sh" "${build_dir}"

catalog_args=("${build_dir}")
[ "${strict}" -eq 1 ] && catalog_args+=(--strict)
VKRELAY2_RENDER_CATALOG_DIR="${artifact_root}/render-catalog" \
    run_stage_with_budget render-catalog "${VKRELAY2_CATALOG_STAGE_TIMEOUT:-900}" \
    bash "${script_dir}/render_catalog.sh" "${catalog_args[@]}"

run_stage lifecycle bash "${script_dir}/run_lifecycle_smoke.sh" "${build_dir}"
run_stage input bash "${script_dir}/run_input_smoke.sh" "${build_dir}"
run_stage resize bash "${script_dir}/run_resize_smoke.sh" "${build_dir}"
run_stage iconify bash "${script_dir}/run_iconify_smoke.sh" "${build_dir}"
run_stage popup bash "${script_dir}/run_popup_smoke.sh" "${build_dir}"
run_stage sync2 bash "${script_dir}/run_vk13_sync2_smoke.sh" "${build_dir}"

echo
echo "SMOKE-GATE: summary pass=${passes} fail=${failures}"
echo "SMOKE-GATE: artifacts preserved in ${artifact_root}"
[ "${failures}" -eq 0 ] || exit 1
echo "SMOKE-GATE: PASS"
exit 0
