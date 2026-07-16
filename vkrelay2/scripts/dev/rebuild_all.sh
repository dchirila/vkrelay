#!/usr/bin/env bash
# vkrelay2: one command to (re)build BOTH toolchains from the WSL side.
#
# vkrelay2 is dual-platform: a Linux ICD/launcher/sidecar (GCC) and a Windows worker/supervisor
# (MSVC). The two trees build with two different toolchains, so there is normally no single "build
# everything" entry point. This script is that entry point -- run it from WSL (Ubuntu):
#   * Linux  -> built natively in WSL via the CMake presets (cmake/ninja/g++ on PATH).
#   * Windows -> built through Windows interop (cmd.exe): it sources vcvars64, prepends the VS-bundled
#                cmake/ninja/clang-format to PATH, kills any running daemon (which would hold the
#                worker .exe lock), and drives the same CMake presets. Nothing needs to be on PATH.
#
# Incremental by default (only changed files recompile); pass --clean for a from-scratch reconfigure.
#
# Usage:
#   scripts/dev/rebuild_all.sh                 # build both (Debug), incremental
#   scripts/dev/rebuild_all.sh --tests         # build both, then ctest both
#   scripts/dev/rebuild_all.sh --lint          # build both, then clang-format --Werror + bash -n
#   scripts/dev/rebuild_all.sh --all           # build + tests + lint (the full dual-platform gate)
#   scripts/dev/rebuild_all.sh --clean         # wipe both build dirs first (full reconfigure)
#   scripts/dev/rebuild_all.sh --release       # use the *-release presets
#   scripts/dev/rebuild_all.sh --linux-only    # skip the Windows half
#   scripts/dev/rebuild_all.sh --windows-only  # skip the Linux half
# Flags combine, e.g.  scripts/dev/rebuild_all.sh --clean --all
set -euo pipefail

# --- options ----------------------------------------------------------------
do_tests=0
do_lint=0
do_clean=0
do_linux=1
do_windows=1
flavor="debug"
for arg in "$@"; do
    case "${arg}" in
        --tests) do_tests=1 ;;
        --lint) do_lint=1 ;;
        --all)
            do_tests=1
            do_lint=1
            ;;
        --clean) do_clean=1 ;;
        --release) flavor="release" ;;
        --linux-only) do_windows=0 ;;
        --windows-only) do_linux=0 ;;
        -h | --help)
            sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "rebuild_all.sh: unknown option '${arg}' (try --help)" >&2
            exit 2
            ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
linux_preset="linux-${flavor}"
windows_preset="windows-${flavor}"
VS="C:\\Program Files\\Microsoft Visual Studio\\18\\Community"
CLANG_FORMAT="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-format.exe"
# /c/... works from Git Bash; from WSL the same exe is under /mnt/c/...
[[ -x "${CLANG_FORMAT}" ]] || CLANG_FORMAT="/mnt/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-format.exe"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok() { printf '\033[1;32m    %s\033[0m\n' "$*"; }
die() {
    printf '\033[1;31mrebuild_all.sh: %s\033[0m\n' "$*" >&2
    exit 1
}

have_windows_interop() { command -v cmd.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; }

# --- Windows-env wrapper: a tiny batch that enters the VS toolchain, then runs one mode ------------
# Written into build/ (gitignored) and removed on exit. Static content (quoted heredoc); the repo
# dir, preset, and mode ride in as %1/%2/%3 so there is no shell/backslash interpolation to mangle.
wrapper_bat=""
cleanup() { [[ -n "${wrapper_bat}" && -f "${wrapper_bat}" ]] && rm -f "${wrapper_bat}"; }
trap cleanup EXIT

write_wrapper() {
    mkdir -p "${repo_root}/build"
    wrapper_bat="${repo_root}/build/.rebuild_all_wrapper.bat"
    cat >"${wrapper_bat}" <<'BATEOF'
@echo off
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%~1" || exit /b 1
if "%~3"=="build" goto :build
if "%~3"=="test" goto :test
echo rebuild_all wrapper: unknown mode "%~3" 1>&2
exit /b 2
:build
rem Free the worker/supervisor .exe locks so linking can overwrite them (best-effort).
taskkill /F /IM vkrelay2-worker.exe >nul 2>&1
taskkill /F /IM vkrelay2-supervisor.exe >nul 2>&1
if not exist "build\%~2\CMakeCache.txt" ( cmake --preset %~2 || exit /b 1 )
cmake --build --preset %~2 || exit /b 1
exit /b 0
:test
ctest --preset %~2 || exit /b 1
exit /b 0
BATEOF
}

run_windows() { # <mode> <preset>
    local mode="$1" preset="$2"
    [[ -n "${wrapper_bat}" ]] || write_wrapper
    local repo_win bat_win
    repo_win="$(wslpath -w "${repo_root}")"
    bat_win="$(wslpath -w "${wrapper_bat}")"
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "${bat_win}" "${repo_win}" "${preset}" "${mode}"
}

# --- clean ------------------------------------------------------------------
if ((do_clean)); then
    step "clean: wiping build dirs"
    ((do_linux)) && rm -rf "${repo_root}/build/${linux_preset}"
    ((do_windows)) && rm -rf "${repo_root}/build/${windows_preset}"
    ok "cleaned"
fi

# --- build ------------------------------------------------------------------
if ((do_linux)); then
    step "build Linux (${linux_preset})"
    (
        cd "${repo_root}"
        if [[ ${do_clean} -eq 1 || ! -f "build/${linux_preset}/CMakeCache.txt" ]]; then
            cmake --preset "${linux_preset}"
        fi
        cmake --build --preset "${linux_preset}"
    )
    ok "Linux build OK"
fi

if ((do_windows)); then
    have_windows_interop || die "no Windows interop (cmd.exe/wslpath) -- run from WSL, or pass --linux-only"
    step "build Windows (${windows_preset})"
    run_windows build "${windows_preset}" || die "Windows build FAILED"
    ok "Windows build OK"
fi

# --- tests ------------------------------------------------------------------
if ((do_tests)); then
    if ((do_linux)); then
        if [[ "${flavor}" == "debug" ]]; then
            step "ctest Linux (${linux_preset})"
            ( cd "${repo_root}" && ctest --preset "${linux_preset}" ) # --preset resolves from cwd
            ok "Linux tests OK"
        else
            echo "    (skipping Linux ctest: no test preset for ${linux_preset})"
        fi
    fi
    if ((do_windows)); then
        if [[ "${flavor}" == "debug" ]]; then
            step "ctest Windows (${windows_preset})"
            run_windows test "${windows_preset}" || die "Windows tests FAILED"
            ok "Windows tests OK"
        else
            echo "    (skipping Windows ctest: no test preset for ${windows_preset})"
        fi
    fi
fi

# --- lint (clang-format --Werror + bash -n) ---------------------------------
if ((do_lint)); then
    step "lint: clang-format --Werror"
    [[ -x "${CLANG_FORMAT}" ]] || die "clang-format not found at ${CLANG_FORMAT}"
    have_windows_interop || die "clang-format needs Windows interop (it is the MSVC LLVM build)"
    # Exempt generated SPIR-V headers (*_spv.h): checked-in glslang output, not hand-maintained
    # source; reformatting them is meaningless and a regeneration would re-dirty them.
    mapfile -t src < <(cd "${repo_root}" && git ls-files '*.cpp' '*.hpp' '*.h' | grep -vE '_spv\.h$')
    win_src=()
    for f in "${src[@]}"; do win_src+=("$(wslpath -w "${repo_root}/${f}")"); done
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "${CLANG_FORMAT}" --dry-run --Werror "${win_src[@]}"
    ok "clang-format clean (${#src[@]} files)"

    step "lint: bash -n (shell scripts)"
    n=0
    while IFS= read -r s; do
        bash -n "${repo_root}/${s}"
        n=$((n + 1))
    done < <(cd "${repo_root}" && git ls-files '*.sh')
    ok "bash -n clean (${n} scripts)"
fi

step "DONE"
summary=""
((do_linux)) && summary+="linux "
((do_windows)) && summary+="windows "
summary+="built"
((do_tests)) && summary+=" + tests"
((do_lint)) && summary+=" + lint"
ok "${summary}"

# the launcher (vkrun) prefers RELEASE artifacts, but
# this gate defaults to DEBUG -- so a green debug gate can leave an existing release tree stale, and
# an app run would then silently execute yesterday's release worker/ICD/sidecar. Nudge at the moment
# the skew is introduced: if we just built DEBUG and a release tree exists that is now OLDER, say so
# (the launcher also warns at bring-up; this catches it one step earlier). Best-effort, never fails.
if [[ "${flavor}" == "debug" ]]; then
    stale_note() { # <debug-artifact> <release-artifact> <label>  (always returns 0: this note is
        # best-effort and must never turn a green gate into exit 1 -- a falsy [[ ]]&& chain as the
        # script's LAST statement did exactly that when the release was NOT stale)
        if [[ -e "$1" && -e "$2" && "$1" -nt "$2" ]]; then
            echo "rebuild_all.sh: NOTE -- the release ${3} is now OLDER than this debug build;" \
                "run 'scripts/dev/rebuild_all.sh --release' before app-testing (vkrun" \
                "prefers release)." >&2
        fi
        return 0
    }
    ((do_linux)) && stale_note \
        "${repo_root}/build/linux-debug/vkrelay2-launch" \
        "${repo_root}/build/linux-release/vkrelay2-launch" "linux stack"
    ((do_windows)) && stale_note \
        "${repo_root}/build/windows-debug/vkrelay2-worker.exe" \
        "${repo_root}/build/windows-release/vkrelay2-worker.exe" "windows worker"
fi
