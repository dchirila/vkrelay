#!/usr/bin/env bash
# vkrelay2 real-application GL/Zink regression smoke.
#
# Synthetic GL canaries deliberately keep their command streams small and deterministic. This
# runner adds the complementary workload shape that exposed non-contiguous command-buffer recording
# epochs: real Mesa/Zink applications through the full vkrun -> private Xwayland -> ICD -> Windows
# worker path.
#
# Required evidence:
#   * glxgears runs for a bounded interval, reports a Zink renderer, and advances its FPS counter;
#   * glmark2's bounded build scene exits cleanly, reports Zink, completes the scene, and scores.
# Both logs must be free of relay/Mesa rejection, crash, and worker-death signatures. A rendered
# frame alone is insufficient: the original regression continued drawing after vkEndCommandBuffer
# had failed.
#
# External app packages are not build dependencies. Missing weston/Xwayland or either app SKIPs
# cleanly; an installed app that starts and then fails is always a failure.
#
# Usage: bash run_gl_apps_smoke.sh [<build-dir>]
#        (build-dir is accepted for aggregate-runner consistency; vkrun locates its own binaries)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"
artifact_root="${VKRELAY2_GL_APPS_LOG_DIR:-${TMPDIR:-/tmp}/vkrelay2-gl-apps-smoke-$$}"

skip() { echo "GL-APPS-SMOKE: SKIP ($1)"; exit 0; }
fail() {
    local label="$1" reason="$2"
    local log="${artifact_root}/${label}.log"
    echo "GL-APPS-SMOKE: FAIL (${label}: ${reason})"
    if [ -r "${log}" ]; then
        echo "    log tail (${log}):"
        tail -40 "${log}" | sed 's/^/        /'
    fi
    echo "GL-APPS-SMOKE: artifacts preserved in ${artifact_root}"
    exit 1
}

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
command -v glxgears >/dev/null 2>&1 || skip "glxgears not installed"
command -v glmark2 >/dev/null 2>&1 || skip "glmark2 not installed"
[ -x "${script_dir}/vkrun" ] || skip "missing ${script_dir}/vkrun"
[ -d "${build_dir}" ] || skip "missing build directory ${build_dir}"

mkdir -p "${artifact_root}"

has_fatal_signature() { # <log>
    grep -Eiq \
        'vkrelay2-icd:.*rejected|MESA:[[:space:]]*error|unimplemented device function|worker (process )?(died|exited unexpectedly)|worker connection closed unexpectedly|Segmentation fault|Aborted|free\(\): invalid pointer|X Error of failed request' \
        "$1"
}

gears_log="${artifact_root}/glxgears.log"
gears_session="${artifact_root}/glxgears-session"
mkdir -p "${gears_session}"
VKRELAY2_LOG_DIR="${gears_session}" timeout --signal=TERM --kill-after=5 20 \
    "${script_dir}/vkrun" -- glxgears -info >"${gears_log}" 2>&1
gears_rc=$?
case "${gears_rc}" in
    124 | 137 | 143) : ;; # expected bounded termination (timeout or vkrun's signal cleanup)
    *) fail glxgears "unexpected exit rc=${gears_rc}" ;;
esac
has_fatal_signature "${gears_log}" && fail glxgears "fatal relay/Mesa/app signature in log"
grep -Eiq 'GL_RENDERER[[:space:]]*[:=].*zink' "${gears_log}" \
    || fail glxgears "renderer evidence is absent or not Zink"
grep -Eq '[1-9][0-9]* frames in [0-9.]+ seconds' "${gears_log}" \
    || fail glxgears "no positive frame/FPS evidence"
echo "GL-APPS-SMOKE: glxgears PASS (bounded Zink run with advancing frames)"

mark_log="${artifact_root}/glmark2.log"
mark_session="${artifact_root}/glmark2-session"
mkdir -p "${mark_session}"
VKRELAY2_LOG_DIR="${mark_session}" timeout --signal=TERM --kill-after=5 180 \
    "${script_dir}/vkrun" -- glmark2 -b build:nframes=100 >"${mark_log}" 2>&1
mark_rc=$?
[ "${mark_rc}" -eq 0 ] || fail glmark2 "unexpected exit rc=${mark_rc}"
has_fatal_signature "${mark_log}" && fail glmark2 "fatal relay/Mesa/app signature in log"
grep -Eiq 'GL_RENDERER[[:space:]]*[:=].*zink' "${mark_log}" \
    || fail glmark2 "renderer evidence is absent or not Zink"
grep -Eq '^\[build\].*FPS:' "${mark_log}" \
    || fail glmark2 "bounded build scene did not complete"
grep -Eq '^[[:space:]]*glmark2 Score:[[:space:]]+[1-9][0-9]*' "${mark_log}" \
    || fail glmark2 "positive score evidence missing"
echo "GL-APPS-SMOKE: glmark2 PASS (bounded build scene, clean exit, positive score)"

echo "============================================================"
echo "GL-APPS-SMOKE: PASS (real glxgears + glmark2 workloads rendered through Zink with no"
echo "               relay/Mesa rejection or crash signatures)"
echo "GL-APPS-SMOKE: artifacts preserved in ${artifact_root}"
echo "============================================================"
exit 0
