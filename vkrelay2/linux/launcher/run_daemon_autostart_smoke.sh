#!/usr/bin/env bash
# vkrelay2 boundary smoke: the WSL -> Windows daemon AUTO-START path must NOT hang.
#
# The field hang was here: with NO daemon running, vkrun's ensure_daemon launches a
# fresh Windows daemon via `powershell.exe ... Start-Process ...`. Because Start-Process with
# -RedirectStandardError/-RedirectStandardOutput forces UseShellExecute=false, the detached daemon
# inherits powershell's WSL-interop pipe, so the `/init` proxy never returns until the daemon EXITS --
# hanging the launcher before its bounded health wait. The fix backgrounds that invocation and reaps the
# proxy after the health check. This smoke proves the auto-start path RETURNS and reaches a healthy
# daemon, on a UNIQUE port so it never disturbs a real daemon on the well-known 13579.
#
# WSL->Windows only: skips cleanly (exit 0) without powershell.exe / wslpath / a Windows build. Cleans up
# ONLY the daemon it started (PID diff), never the user's.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
launcher="${script_dir}/vkrun"
port="${VKRELAY2_AUTOSTART_SMOKE_PORT:-13601}"
deadline="${VKRELAY2_AUTOSTART_DEADLINE:-90}"

skip() { echo "AUTOSTART-SMOKE: SKIP ($1)"; exit 0; }

command -v powershell.exe >/dev/null 2>&1 || skip "no powershell.exe (not a WSL->Windows host)"
command -v wslpath >/dev/null 2>&1 || skip "no wslpath"
[ -x "${launcher}" ] || skip "launcher not found: ${launcher}"
# Confirm a Windows build exists (else the launcher can't auto-start a daemon and would mock-fall-back).
win_build=""
for d in "${VKRELAY2_WIN_BUILD:-}" "${script_dir}/../../build/windows-debug" "${script_dir}/../../build/windows-release"; do
    [ -n "${d}" ] && [ -x "${d}/vkrelay2-supervisor.exe" ] && { win_build="${d}"; break; }
done
[ -n "${win_build}" ] || skip "no Windows build (vkrelay2-supervisor.exe) for the auto-start path"

# PID set of supervisors BEFORE (so we kill ONLY the one we start, never the user's daemon on 13579).
sup_pids() { powershell.exe -NoProfile -NonInteractive -Command \
    "Get-Process vkrelay2-supervisor -ErrorAction SilentlyContinue | ForEach-Object { \$_.Id }" \
    </dev/null 2>/dev/null | tr -d '\r'; }
before="$(sup_pids)"

cleanup() {
    # Kill ONLY supervisors that appeared during this smoke (after-set minus before-set).
    local after; after="$(sup_pids)"
    local p
    for p in ${after}; do
        case " ${before} " in *" ${p} "*) : ;; *)
            powershell.exe -NoProfile -NonInteractive -Command "Stop-Process -Id ${p} -Force -ErrorAction SilentlyContinue" </dev/null >/dev/null 2>&1 || true
            ;;
        esac
    done
}
trap cleanup EXIT

echo "AUTOSTART-SMOKE: running --list-gpus with NO daemon on unique port ${port} (auto-start path)"
err="$(mktemp)"
t0=${SECONDS}
out="$(timeout "${deadline}" env VKRELAY2_DAEMON_PORT="${port}" bash "${launcher}" --list-gpus 2>"${err}")"
rc=$?
elapsed=$((SECONDS - t0))

if [ "${rc}" -eq 124 ]; then
    echo "AUTOSTART-SMOKE: FAIL (launcher HUNG -- timed out after ${deadline}s on the auto-start path)"
    echo "--- stderr tail ---"; tail -15 "${err}" 2>/dev/null
    rm -f "${err}"; exit 1
fi
# The auto-start phase must have reached a healthy daemon (not the mock fallback) -- the proof the
# powershell Start-Process returned control and the health wait succeeded.
if ! grep -q "daemon: started + healthy" "${err}"; then
    echo "AUTOSTART-SMOKE: FAIL (no 'daemon: started + healthy' phase -- auto-start did not complete in ${elapsed}s, rc=${rc})"
    echo "--- stderr tail ---"; tail -20 "${err}" 2>/dev/null
    rm -f "${err}"; exit 1
fi
if printf '%s' "${out}" | grep -q "local mock list"; then
    echo "AUTOSTART-SMOKE: FAIL (fell back to the mock list -- the daemon was not actually reached)"
    rm -f "${err}"; exit 1
fi
rm -f "${err}"

echo "============================================================"
echo "AUTOSTART-SMOKE: PASS (fresh Windows daemon auto-started + healthy in ${elapsed}s, launcher"
echo "                 returned cleanly -- no powershell/interop hang)"
echo "============================================================"
exit 0
