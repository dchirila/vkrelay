#!/usr/bin/env bash
# vkrelay2 boundary smoke: the profiling dumps EXIST on BOTH ends and stay parseable.
# Runs two canaries through the full relay with VKRELAY2_PROFILE=1:
#   1. vkrelay2-readback-canary -- a short deterministic op mix (renders, copies, readbacks);
#   2. vkrelay2-canary (the native clear canary) -- presents real frames, so the client dump
#      carries a frames= summary (present-to-present timing).
# Asserts: each canary SUCCEEDS (rc==0 + its own PASS marker -- a measurement gate must prove it
# measured successful executions); client dump lines in the app output; worker
# dump lines in the captured daemon worker log (the stream-proof obligation -- worker
# stderr must actually reach the redirected log); and scripts/dev/profile_report.sh STRICT-parses
# the combined logs into non-empty op tables. The parser is strict by design: dump-grammar drift
# fails HERE, not silently in 6.2.
#
# Determinism WITHOUT disturbing the user's daemon: this smoke OWNS its daemon.
# It runs on a smoke-only port with a smoke-only worker-log path (VKRELAY2_WORKER_LOG), so the
# launcher auto-starts a fresh daemon whose stderr redirect is at a path this smoke controls; a
# PID-diff cleanup then kills ONLY the supervisor/worker processes that appeared during the smoke
# (the run_daemon_autostart_smoke.sh pattern). The user's default daemon on 13579 -- and its log --
# are never touched.
#
# REAL backend required; SKIPs cleanly when this is not a GPU/daemon box.
#
# Usage: bash run_profile_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"
port="${VKRELAY2_PROFILE_SMOKE_PORT:-13607}"

skip() { echo "PROFILE-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
command -v powershell.exe >/dev/null 2>&1 || skip "no powershell.exe (not a WSL->Windows host)"
rb_canary="${build_dir}/vkrelay2-readback-canary"
clear_canary="${build_dir}/vkrelay2-canary"
report="${script_dir}/../../scripts/dev/profile_report.sh"
[ -x "${rb_canary}" ] || skip "missing ${rb_canary} (build the linux preset; needs gl/x11 dev)"
[ -x "${clear_canary}" ] || skip "missing ${clear_canary}"
[ -r "${report}" ] || { echo "PROFILE-SMOKE: FAIL (missing ${report})"; exit 1; }

win_build="${script_dir}/../../build/windows-debug"
if [ ! -x "${win_build}/vkrelay2-supervisor.exe" ]; then
    skip "no Windows build (not a GPU/daemon box)"
fi
# Smoke-owned worker log: the launcher redirects the daemon it starts for THIS port here, so the
# worker-log proof never depends on (or clobbers) the default daemon's vkrelay2-worker-daemon.log.
worker_log="${win_build}/vkrelay2-worker-daemon.profile-smoke.log"
: > "${worker_log}" 2>/dev/null || true

# PID set of relay processes BEFORE, so cleanup kills ONLY what this smoke started (never the
# user's daemon). Workers included: the owned daemon spawns one per canary session.
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

echo "PROFILE-SMOKE: using an owned daemon on port ${port} (worker log: ${worker_log##*/})"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/vkrelay2-profile-smoke-XXXXXX")"
overall_rc=0

run_one() { # <name> <binary> <pass-marker or "">
    local name="$1" binary="$2" marker="$3"
    local out rc
    out="$(VKRELAY2_PROFILE=1 VKRELAY2_PROFILE_FRAMES=1 VKRELAY2_DAEMON_PORT="${port}" \
        VKRELAY2_WORKER_LOG="${worker_log}" timeout 120 \
        "${script_dir}/vkrun" -- "${binary}" 2>&1)"
    rc=$?
    printf '%s\n' "${out}" > "${run_dir}/${name}.log"
    if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
        skip "no real daemon (not a GPU box)"
    fi
    # The canary run itself must SUCCEED: profile lines from a crashed or
    # timed-out run are not proof the instrumentation measured a working relay.
    if [ "${rc}" -eq 124 ]; then
        echo "PROFILE-SMOKE: FAIL (${name}: TIMED OUT after 120s)"
        overall_rc=1
        return
    fi
    if [ "${rc}" -ne 0 ]; then
        echo "PROFILE-SMOKE: FAIL (${name}: canary rc=${rc})"
        overall_rc=1
        return
    fi
    if [ -n "${marker}" ] && ! printf '%s\n' "${out}" | grep -q "${marker}"; then
        echo "PROFILE-SMOKE: FAIL (${name}: rc=0 but no '${marker}' line)"
        overall_rc=1
        return
    fi
    if ! printf '%s\n' "${out}" | grep -q "VKRELAY2-PROFILE end=client"; then
        echo "PROFILE-SMOKE: FAIL (${name}: canary succeeded but no client dump lines)"
        overall_rc=1
    fi
}

run_one readback "${rb_canary}" "READBACK-CANARY: PASS"
run_one clear "${clear_canary}" "" # its success contract is rc==0 + the frames= check below
[ "${overall_rc}" -eq 0 ] || exit 1
echo "PROFILE-SMOKE: both canaries succeeded with client dump lines"

# The clear canary presents real frames -> the client dump must carry frame aggregates.
if ! grep -qE "VKRELAY2-PROFILE end=client frames=[1-9]" "${run_dir}/clear.log"; then
    echo "PROFILE-SMOKE: FAIL (clear canary produced no frames= summary)"
    exit 1
fi

# Stream proof: the worker's stderr dump must actually reach the captured daemon log.
if ! grep -q "VKRELAY2-PROFILE end=worker" "${worker_log}" 2>/dev/null; then
    echo "PROFILE-SMOKE: FAIL (no worker dump lines in ${worker_log} -- the profiling stream path is broken;"
    echo "               fallbacks per the locked design: inherit_streams for session workers or"
    echo "               per-end VKRELAY2_PROFILE_OUT files)"
    exit 1
fi
echo "PROFILE-SMOKE: worker dump lines present in the owned daemon's worker log"

# the worker record handler reports its internal phase split (json_parse /
# decode+hex / validate / replay / execute). The canaries record command streams, so the line
# must exist -- this pins the phase timers against silent regression.
if ! grep -q "record_phases=" "${worker_log}" 2>/dev/null; then
    echo "PROFILE-SMOKE: FAIL (no record_phases line in the worker dump -- phase timers missing)"
    exit 1
fi
echo "PROFILE-SMOKE: worker record-phase split present"

# The summarizer STRICT-parses the combined logs into a non-empty op table.
report_out="$(bash "${report}" "${run_dir}/readback.log" "${run_dir}/clear.log" "${worker_log}" 2>&1)"
report_rc=$?
printf '%s\n' "${report_out}" | head -25 | sed 's/^/    /'
if [ "${report_rc}" -ne 0 ]; then
    echo "PROFILE-SMOKE: FAIL (profile_report.sh rejected the dumps, rc=${report_rc})"
    exit 1
fi
if ! printf '%s\n' "${report_out}" | grep -q "client op table"; then
    echo "PROFILE-SMOKE: FAIL (report has no client op table)"
    exit 1
fi
if ! printf '%s\n' "${report_out}" | grep -q "worker op table"; then
    echo "PROFILE-SMOKE: FAIL (report has no worker op table)"
    exit 1
fi

# Preserve the profiling evidence for later comparison.
cp "${worker_log}" "${run_dir}/worker.log" 2>/dev/null || true
echo "PROFILE-SMOKE: logs preserved in ${run_dir}"
echo "============================================================"
echo "PROFILE-SMOKE: PASS (canaries succeeded, client + worker dumps present, frames summarized,"
echo "               report parses; owned daemon on port ${port}, user daemon untouched)"
echo "============================================================"
exit 0
