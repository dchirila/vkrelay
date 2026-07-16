#!/usr/bin/env bash
# The profiling MATRIX runner: every optimization change runs this
# before and after, so gains remain attributable across the same fixed application set.
#
# Runs the 6.2 matrix through the full relay with VKRELAY2_PROFILE=1 VKRELAY2_PROFILE_FRAMES=1:
#   vkcube --c 300                          native Vulkan, minimal per-frame op mix
#   glxgears (30 s)                         GL->zink, huge per-frame record streams
#   glmark2 -b build -b texture -b shading  GL->zink benchmark (score = the headline number)
#   openscad CSG.scad GUI (45 s)            real Qt app profile
#   openscad -o png CSG.scad                offscreen FBO + readback path
#
# One OWNED daemon (unique port + owned worker log, PID-diff cleanup -- the run_profile_smoke.sh
# ownership pattern; the user's daemon and log are never touched). Per-app worker dumps are
# extracted from the redirect by byte-offset delta. Each app also gets a profile_report.sh
# render. Timed-out apps (glxgears, the GUI) dump their CLIENT end via the ICD's signal-time
# dump into per-app VKRELAY2_PROFILE_OUT files.
#
# This is a measurement tool, not a numeric gate -- but EVIDENCE COMPLETENESS is a hard contract
# a missing client dump, a missing worker dump, or a summarizer parse
# failure on any non-skipped entry exits nonzero. SKIPs cleanly when this is not a GPU/daemon
# box.
#
# Usage: bash run_profile_matrix.sh [<out-dir>] [<build-dir>]
#   out-dir default: a fresh mktemp -d; build-dir default: ../../build/linux-debug
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
out="${1:-}"
build_dir="${2:-${script_dir}/../../build/linux-debug}"
port="${VKRELAY2_MATRIX_PORT:-13613}"
report="${script_dir}/../../scripts/dev/profile_report.sh"

skip() { echo "PROFILE-MATRIX: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
command -v powershell.exe >/dev/null 2>&1 || skip "no powershell.exe (not a WSL->Windows host)"
[ -r "${report}" ] || { echo "PROFILE-MATRIX: FAIL (missing ${report})"; exit 1; }
win_build="${script_dir}/../../build/windows-debug"
[ -x "${win_build}/vkrelay2-supervisor.exe" ] || skip "no Windows build (not a GPU/daemon box)"

[ -n "${out}" ] || out="$(mktemp -d "${TMPDIR:-/tmp}/vkrelay2-profile-matrix-XXXXXX")"
mkdir -p "${out}"
worker_log="${win_build}/vkrelay2-worker-daemon.profile-matrix.log"
: > "${worker_log}" 2>/dev/null || true

vkr_pids() { powershell.exe -NoProfile -NonInteractive -Command \
    "Get-Process vkrelay2-supervisor,vkrelay2-worker -ErrorAction SilentlyContinue | ForEach-Object { \$_.Id }" \
    </dev/null 2>/dev/null | tr -d '\r'; }
before_pids="$(vkr_pids)"
cleanup() {
    local after p
    after="$(vkr_pids)"
    for p in ${after}; do
        case " ${before_pids} " in *" ${p} "*) : ;; *)
            powershell.exe -NoProfile -NonInteractive -Command \
                "Stop-Process -Id ${p} -Force -ErrorAction SilentlyContinue" \
                </dev/null >/dev/null 2>&1 || true
            ;;
        esac
    done
}
trap cleanup EXIT

echo "PROFILE-MATRIX: owned daemon on port ${port}; evidence -> ${out}"
missing_client=""
matrix_rc=0

run_one() { # <name> <timeout-s> <cmd...>
    local name="$1" to="$2"
    shift 2
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "PROFILE-MATRIX: ${name}: SKIPPED (missing $1)"
        return
    fi
    local pre post rc t0
    pre=$(stat -c %s "${worker_log}" 2>/dev/null || echo 0)
    echo "=== ${name}: $* (timeout ${to}s) ==="
    t0=${SECONDS}
    # VKRELAY2_PROFILE_OUT (a real file, opened by the dump itself): timeout(1) group-kills the
    # WHOLE pipeline, so the tee reading the app's stderr dies with the app -- a signal-time dump
    # written to fd 2 vanishes into a dead pipe (EPIPE). The file path is immune. Client dumps
    # land in <name>.profile.client.<pid>; the worker end keeps its daemon-log redirect.
    # -k 10: if teardown wedges after TERM, KILL after 10s so the matrix never hangs.
    VKRELAY2_PROFILE=1 VKRELAY2_PROFILE_FRAMES=1 VKRELAY2_DAEMON_PORT="${port}" \
        VKRELAY2_WORKER_LOG="${worker_log}" VKRELAY2_PROFILE_OUT="${out}/${name}.profile" \
        timeout -k 10 "${to}" \
        "${script_dir}/vkrun" -- "$@" >"${out}/${name}.app.log" 2>&1
    rc=$?
    echo "rc=${rc} wall_s=$((SECONDS - t0))" | tee "${out}/${name}.meta"
    if grep -qE "no daemon reachable and no Windows build found" "${out}/${name}.app.log"; then
        skip "no real daemon (not a GPU box)"
    fi
    sleep 3 # session close -> worker process exit -> its dump lands in the redirect
    post=$(stat -c %s "${worker_log}" 2>/dev/null || echo 0)
    tail -c +$((pre + 1)) "${worker_log}" | head -c $((post - pre)) > "${out}/${name}.worker.log"
    local client_files=()
    local f
    for f in "${out}/${name}.profile".client.*; do
        [ -r "$f" ] && client_files+=("$f")
    done
    if ! cat "${client_files[@]:-/dev/null}" 2>/dev/null | grep -q "VKRELAY2-PROFILE end=client"; then
        # Evidence completeness is a HARD contract: a before/after run that
        # silently lost the client end of a non-skipped entry must not look green.
        echo "PROFILE-MATRIX: ${name}: MISSING-CLIENT-DUMP (signal-time dump did not fire?)"
        missing_client="${missing_client} ${name}"
        matrix_rc=1
    fi
    if ! grep -q "VKRELAY2-PROFILE end=worker" "${out}/${name}.worker.log"; then
        echo "PROFILE-MATRIX: ${name}: MISSING-WORKER-DUMP (session did not close cleanly?)"
        matrix_rc=1
    fi
    bash "${report}" "${client_files[@]}" "${out}/${name}.worker.log" \
        > "${out}/${name}.report.txt" 2>&1 || {
        echo "PROFILE-MATRIX: ${name}: report FAILED to parse (grammar drift?)"
        matrix_rc=1
    }
    grep -E "^frames=|glmark2 Score|frames in .* seconds" \
        "${out}/${name}.report.txt" "${out}/${name}.app.log" 2>/dev/null | head -4 | sed 's/^/    /'
}

scad="/usr/share/openscad/examples/Basics/CSG.scad"
run_one vkcube 240 vkcube --c 300
run_one glxgears 30 glxgears
run_one glmark2 300 glmark2 -b build -b texture -b shading
if [ -r "${scad}" ]; then
    run_one openscad_gui 45 openscad "${scad}"
    run_one openscad_png 240 openscad -o "${out}/csg-export.png" "${scad}"
else
    echo "PROFILE-MATRIX: openscad runs SKIPPED (no ${scad})"
fi

echo "============================================================"
if [ -n "${missing_client}" ]; then
    echo "PROFILE-MATRIX: WARNING -- no client dump for:${missing_client}"
fi
if [ "${matrix_rc}" -eq 0 ]; then
    echo "PROFILE-MATRIX: DONE (per-app app.log/worker.log/report.txt in ${out})"
else
    echo "PROFILE-MATRIX: DONE WITH ERRORS (see above; evidence in ${out})"
fi
echo "============================================================"
exit "${matrix_rc}"
