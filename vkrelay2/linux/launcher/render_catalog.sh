#!/usr/bin/env bash
# vkrelay2 final-presentation render catalog.
#
# Runs real applications through vkrun one at a time, captures the resulting Windows worker HWND,
# and requires one chromed top-level with a non-degenerate client image. This complements (and does
# not replace) the deterministic self-judging GL/Vulkan canaries.
#
# Usage:
#   bash render_catalog.sh [<build-dir>] [--strict] [--case <name>]
#
# Default mode SKIPs an external application that is not installed. --strict makes every selected
# catalog row a required dependency. Available cases: vkcube, glxgears, glmark2, openscad.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
build_dir="${repo_root}/build/linux-debug"
strict=0
selected_case=""

if [ "${1:-}" != "" ] && [ "${1#--}" = "${1}" ]; then
    build_dir="$1"
    shift
fi
while [ "$#" -gt 0 ]; do
    case "$1" in
        --strict) strict=1 ;;
        --case)
            shift
            [ "$#" -gt 0 ] || { echo "RENDER-CATALOG: FAIL (--case needs a name)"; exit 2; }
            selected_case="$1"
            ;;
        -h | --help)
            sed -n '2,/^set -u$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "RENDER-CATALOG: FAIL (unknown option '$1')"
            exit 2
            ;;
    esac
    shift
done

artifact_root="${VKRELAY2_RENDER_CATALOG_DIR:-${TMPDIR:-/tmp}/vkrelay2-render-catalog-$$}"
capture_ps="${repo_root}/scripts/dev/capture_window.ps1"
vkrun="${script_dir}/vkrun"
failures=0
passes=0
skips=0

missing_platform() { # <reason>
    if [ "${strict}" -eq 1 ]; then
        echo "RENDER-CATALOG: FAIL ($1)"
        exit 1
    fi
    echo "RENDER-CATALOG: SKIP ($1)"
    exit 0
}

command -v powershell.exe >/dev/null 2>&1 ||
    missing_platform "powershell.exe unavailable; requires WSL -> Windows interop"
command -v wslpath >/dev/null 2>&1 ||
    missing_platform "wslpath unavailable; requires WSL"
for tool in weston Xwayland; do
    command -v "${tool}" >/dev/null 2>&1 || missing_platform "${tool} not found"
done
[ -x "${vkrun}" ] || { echo "RENDER-CATALOG: FAIL (missing ${vkrun})"; exit 1; }
[ -r "${capture_ps}" ] || { echo "RENDER-CATALOG: FAIL (missing ${capture_ps})"; exit 1; }
[ -d "${build_dir}" ] || missing_platform "missing build directory ${build_dir}"
mkdir -p "${artifact_root}"

selected() { [ -z "${selected_case}" ] || [ "${selected_case}" = "$1" ]; }

missing_app() { # <label> <guidance>
    local label="$1" guidance="$2"
    if [ "${strict}" -eq 1 ]; then
        echo "RENDER-CATALOG: FAIL (${label}: dependency missing; ${guidance})"
        failures=$((failures + 1))
    else
        echo "RENDER-CATALOG: SKIP (${label}: ${guidance})"
        skips=$((skips + 1))
    fi
}

fatal_log_signature() { # <log>
    grep -Eiq \
        'vkrelay2-icd:.*rejected|MESA:[[:space:]]*error|unimplemented device function|worker (process )?(died|exited unexpectedly)|worker connection closed unexpectedly|Segmentation fault|Aborted|free\(\): invalid pointer|X Error of failed request' \
        "$1"
}

run_case() { # <label> <min-colors> <exit-mode:terminate|clean> <command> [args...]
    local label="$1" threshold="$2" exit_mode="$3"
    shift 3
    local case_dir="${artifact_root}/${label}"
    local app_log="${case_dir}/app.log"
    local capture_log="${case_dir}/capture.log"
    local png="${case_dir}/window.png"
    local json="${case_dir}/window.json"
    local app_pid app_rc capture_rc result unique

    mkdir -p "${case_dir}/session"
    echo "RENDER-CATALOG: ${label}: launching (minimum ${threshold} sparse-grid colors)"
    VKRELAY2_LOG_DIR="${case_dir}/session" \
        timeout --signal=TERM --kill-after=5 180 "${vkrun}" -- "$@" >"${app_log}" 2>&1 &
    app_pid=$!

    powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass \
        -File "$(wslpath -w "${capture_ps}")" \
        -OutputPng "$(wslpath -w "${png}")" \
        -EmitJson "$(wslpath -w "${json}")" \
        -ExpectedWindowCount 1 \
        -AllowForegroundFallback \
        -MinUniqueColors "${threshold}" \
        -CaptureAttempts 3 \
        -RetryDelayMs 750 \
        -MaxWaitSec 20 \
        -WarmupSec 2 >"${capture_log}" 2>&1
    capture_rc=$?

    if [ "${exit_mode}" = "terminate" ]; then
        kill "${app_pid}" 2>/dev/null || true
    fi
    wait "${app_pid}" 2>/dev/null
    app_rc=$?

    result="$(tr -d '\r' <"${capture_log}" | grep '^RESULT ' | tail -1)"
    if [ "${capture_rc}" -ne 0 ] || ! printf '%s\n' "${result}" | grep -q 'status=ok'; then
        echo "RENDER-CATALOG: FAIL (${label}: HWND capture/topology gate failed, rc=${capture_rc})"
        sed 's/^/    /' "${capture_log}"
        failures=$((failures + 1))
        return
    fi
    if ! printf '%s\n' "${result}" | grep -q 'chrome=True' ||
       ! printf '%s\n' "${result}" | grep -q 'window_count=1'; then
        echo "RENDER-CATALOG: FAIL (${label}: expected one chromed worker top-level)"
        echo "    ${result}"
        failures=$((failures + 1))
        return
    fi
    unique="$(printf '%s\n' "${result}" | sed -n 's/.*unique_colors=\([0-9][0-9]*\).*/\1/p')"
    if [ -z "${unique}" ] || [ "${unique}" -lt "${threshold}" ]; then
        echo "RENDER-CATALOG: FAIL (${label}: unique-color oracle missing/below ${threshold})"
        failures=$((failures + 1))
        return
    fi
    if fatal_log_signature "${app_log}"; then
        echo "RENDER-CATALOG: FAIL (${label}: fatal relay/Mesa/app signature in ${app_log})"
        grep -Ei \
            'vkrelay2-icd:.*rejected|MESA:[[:space:]]*error|unimplemented|worker .*died|worker .*exited unexpectedly|Segmentation fault|Aborted|free\(\): invalid pointer|X Error' \
            "${app_log}" | tail -20 | sed 's/^/    /'
        failures=$((failures + 1))
        return
    fi
    if [ "${exit_mode}" = "clean" ] && [ "${app_rc}" -ne 0 ]; then
        echo "RENDER-CATALOG: FAIL (${label}: expected clean app exit, rc=${app_rc})"
        failures=$((failures + 1))
        return
    fi

    echo "RENDER-CATALOG: ${label}: PASS (${unique} colors; ${result#RESULT })"
    passes=$((passes + 1))
    sleep 1 # allow the prior session worker HWND to disappear before the next topology assertion
}

if selected vkcube; then
    vkcube_bin="${VKRELAY2_VKCUBE:-}"
    if [ -z "${vkcube_bin}" ]; then
        for candidate in vkcube vkcube-wayland /usr/bin/vkcube /usr/local/bin/vkcube; do
            if command -v "${candidate}" >/dev/null 2>&1; then
                vkcube_bin="$(command -v "${candidate}")"
                break
            fi
        done
    fi
    if [ -n "${vkcube_bin}" ] && [ -x "${vkcube_bin}" ]; then
        run_case vkcube 16 terminate "${vkcube_bin}" --c 1000 --suppress_popups
    else
        missing_app vkcube "install vulkan-tools or set VKRELAY2_VKCUBE"
    fi
fi

if selected glxgears; then
    if command -v glxgears >/dev/null 2>&1; then
        run_case glxgears 40 terminate glxgears
    else
        missing_app glxgears "install mesa-utils"
    fi
fi

if selected glmark2; then
    if command -v glmark2 >/dev/null 2>&1; then
        run_case glmark2 32 clean glmark2 -b build:nframes=500
    else
        missing_app glmark2 "install glmark2"
    fi
fi

if selected openscad; then
    scad="${VKRELAY2_OPENSCAD_MODEL:-/usr/share/openscad/examples/Basics/CSG.scad}"
    if command -v openscad >/dev/null 2>&1 && [ -r "${scad}" ]; then
        run_case openscad 16 terminate openscad "${scad}"
    else
        missing_app openscad "install openscad with examples, or set VKRELAY2_OPENSCAD_MODEL"
    fi
fi

if [ -n "${selected_case}" ]; then
    case "${selected_case}" in
        vkcube | glxgears | glmark2 | openscad) : ;;
        *)
            echo "RENDER-CATALOG: FAIL (unknown case '${selected_case}')"
            exit 2
            ;;
    esac
fi

echo "RENDER-CATALOG: summary pass=${passes} skip=${skips} fail=${failures}"
echo "RENDER-CATALOG: artifacts preserved in ${artifact_root}"
[ "${failures}" -eq 0 ] || exit 1
[ "${passes}" -gt 0 ] || {
    echo "RENDER-CATALOG: SKIP (no selected catalog application is available)"
    exit 0
}
echo "RENDER-CATALOG: PASS"
exit 0
