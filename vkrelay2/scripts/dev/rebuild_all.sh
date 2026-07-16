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
#   scripts/dev/rebuild_all.sh --check-deps    # preflight only; do not clean/build/test/lint
# Flags combine, e.g.  scripts/dev/rebuild_all.sh --clean --all
# This script never builds the separate private-Xwayland stage, but its Linux preflight requires a
# compatible stage so a green rebuild is ready for app runs/release packaging. --windows-only skips
# that Linux runtime-readiness gate; see docs/building.md#private-xwayland.
set -euo pipefail

# --- options ----------------------------------------------------------------
do_tests=0
do_lint=0
do_clean=0
do_linux=1
do_windows=1
flavor="debug"
check_deps_only=0
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
        --check-deps) check_deps_only=1 ;;
        -h | --help)
            sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
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

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok() { printf '\033[1;32m    %s\033[0m\n' "$*"; }
die() {
    printf '\033[1;31mrebuild_all.sh: %s\033[0m\n' "$*" >&2
    exit 1
}

# --- Visual Studio detection -------------------------------------------------
# The VS install root differs by edition (Community at home, Professional/Enterprise on work
# machines, BuildTools on agents). Honor an explicit VKRELAY2_VS_DIR (Windows path), else probe
# the standard editions in order. VS holds the Windows path (used inside the batch wrapper);
# VS_UNIX the same root as this shell sees it (/c/... from Git Bash, /mnt/c/... from WSL).
vs_editions=(Community Professional Enterprise BuildTools)
VS=""
VS_UNIX=""
win_to_unix() { # C:\... -> existing unix path on stdout, or empty
    local p tool
    for tool in wslpath cygpath; do
        command -v "${tool}" >/dev/null 2>&1 || continue
        p="$("${tool}" -u "$1" 2>/dev/null || true)"
        [[ -n "${p}" && -e "${p}" ]] && { printf '%s\n' "${p}"; return 0; }
    done
    return 0
}
if [[ -n "${VKRELAY2_VS_DIR:-}" ]]; then
    VS="${VKRELAY2_VS_DIR%[\\/]}"
    VS_UNIX="$(win_to_unix "${VS}")"
else
    for ed in "${vs_editions[@]}"; do
        for prefix in /c /mnt/c; do
            if [[ -e "${prefix}/Program Files/Microsoft Visual Studio/18/${ed}/VC/Auxiliary/Build/vcvars64.bat" ]]; then
                VS="C:\\Program Files\\Microsoft Visual Studio\\18\\${ed}"
                VS_UNIX="${prefix}/Program Files/Microsoft Visual Studio/18/${ed}"
                break 2
            fi
        done
    done
fi
CLANG_FORMAT="${VS_UNIX}/VC/Tools/Llvm/x64/bin/clang-format.exe"

require_vs() { # only the Windows build and lint need VS; --linux-only must not care
    [[ -n "${VS}" ]] || die "no Visual Studio 2026 found under 'C:\\Program Files\\Microsoft Visual Studio\\18\\{${vs_editions[*]// /,}}' -- set VKRELAY2_VS_DIR to its install root"
}

have_windows_interop() { command -v cmd.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; }
windows_interop_executes() { cmd.exe /d /c ver </dev/null >/dev/null 2>&1; }

windows_interop_guidance() {
    cat >&2 <<'EOF'

Windows dependency preflight FAILED: WSL cannot execute Windows programs.
cmd.exe is on PATH, but its execution failed (the cause of "cannot execute binary file:
Exec format error").

Restart WSL completely from Windows PowerShell or Command Prompt:
  wsl --shutdown

Then reopen this distribution and retry the command.

More detail: docs/troubleshooting.md#daemon-auto-start-fails-windows-interop-broken
EOF
}

# --- Windows-env wrapper: a tiny batch that enters the VS toolchain, then runs one mode ------------
# Written into build/ (gitignored) and removed on exit. Static content (quoted heredoc); the repo
# dir, preset, mode, VS root, and probe switches ride in as arguments so there is no
# shell/backslash interpolation to mangle.
wrapper_bat=""
cleanup() {
    [[ -n "${wrapper_bat}" && -f "${wrapper_bat}" ]] && rm -f "${wrapper_bat}"
    return 0
}
trap cleanup EXIT

write_wrapper() {
    mkdir -p "${repo_root}/build"
    wrapper_bat="${repo_root}/build/.rebuild_all_wrapper.bat"
    cat >"${wrapper_bat}" <<'BATEOF'
@echo off
setlocal
set "VS=%~4"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo   - Could not initialize the VS x64 C++ environment from "%VS%". 1>&2
    exit /b 1
)
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%~1" || exit /b 1
if "%~3"=="build" goto :build
if "%~3"=="test" goto :test
if "%~3"=="probe" goto :probe
echo rebuild_all wrapper: unknown mode "%~3" 1>&2
exit /b 2
:probe
set "FAILED="
if "%~5"=="1" (
    where cl.exe >nul 2>&1 || (
        echo   - MSVC x64/x86 compiler not found. 1>&2
        echo     Visual Studio Installer -^> Modify -^> Individual components: 1>&2
        echo       "MSVC Build Tools for x64/x86 (Latest)" 1>&2
        set "FAILED=1"
    )
    if not exist "%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        echo   - VS-bundled CMake not found. 1>&2
        echo     Visual Studio Installer -^> Modify -^> Individual components: 1>&2
        echo       "C++ CMake tools for Windows" 1>&2
        set "FAILED=1"
    )
    if not exist "%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
        echo   - VS-bundled Ninja not found. 1>&2
        echo     Visual Studio Installer -^> Modify -^> Individual components: 1>&2
        echo       "C++ CMake tools for Windows" 1>&2
        set "FAILED=1"
    )
    if not defined WindowsSdkDir (
        echo   - Windows SDK environment not found. 1>&2
        echo     Visual Studio Installer -^> Modify -^> Individual components: "Windows 11 SDK" 1>&2
        set "FAILED=1"
    ) else (
        if not exist "%WindowsSdkDir%Include\%WindowsSDKVersion%um\Windows.h" (
            echo   - Windows SDK headers not found. 1>&2
            echo     Repair/install "Windows 11 SDK" in Visual Studio Installer. 1>&2
            set "FAILED=1"
        )
        if not exist "%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64\Shcore.lib" (
            echo   - Windows SDK x64 libraries not found. 1>&2
            echo     Repair/install "Windows 11 SDK" in Visual Studio Installer. 1>&2
            set "FAILED=1"
        )
    )
    if not defined VULKAN_SDK (
        echo   - VULKAN_SDK is not visible. 1>&2
        echo     Install the LunarG Vulkan SDK for Windows x64 with Core only. 1>&2
        echo     On "Select Components", click "None"; Core remains installed automatically. 1>&2
        echo     Default location: C:\VulkanSDK\[version]. Restart Windows after installation. 1>&2
        set "FAILED=1"
    ) else (
        if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
            echo   - Vulkan headers missing at %VULKAN_SDK%\Include\vulkan\vulkan.h. 1>&2
            echo     Repair/install the LunarG Vulkan SDK for Windows x64 with Core only. 1>&2
            echo     On "Select Components", click "None"; Core remains installed automatically. 1>&2
            set "FAILED=1"
        )
        if not exist "%VULKAN_SDK%\Lib\vulkan-1.lib" (
            echo   - Vulkan loader import library missing at %VULKAN_SDK%\Lib\vulkan-1.lib. 1>&2
            echo     Repair/install the LunarG Vulkan SDK for Windows x64 with Core only. 1>&2
            echo     On "Select Components", click "None"; Core remains installed automatically. 1>&2
            set "FAILED=1"
        )
    )
)
if "%~6"=="1" if not exist "%VS%\VC\Tools\Llvm\x64\bin\clang-format.exe" (
    echo   - clang-format not found. 1>&2
    echo     Visual Studio Installer -^> Modify -^> Individual components: 1>&2
    echo       "C++ Clang tools for Windows" 1>&2
    set "FAILED=1"
)
if defined FAILED exit /b 1
exit /b 0
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

run_windows() { # <mode> <preset> [extra1] [extra2]
    local mode="$1" preset="$2" extra1="${3:-}" extra2="${4:-}"
    require_vs
    [[ -n "${wrapper_bat}" ]] || write_wrapper
    local repo_win bat_win
    repo_win="$(wslpath -w "${repo_root}")"
    bat_win="$(wslpath -w "${wrapper_bat}")"
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "${bat_win}" "${repo_win}" "${preset}" "${mode}" "${VS}" "${extra1}" "${extra2}"
}

# --- dependency preflight ---------------------------------------------------
# CMake deliberately permits reduced builds when optional libraries are absent. This entry point is
# the complete two-platform gate, so fail before cleaning/configuring if either product would be
# incomplete. Keep the install recipe beside the checks so a fresh machine gets one useful answer.
linux_dependency_preflight() {
    local missing=() cmd spec module label
    for spec in \
        "cmake:CMake" \
        "ninja:Ninja" \
        "g++:the GNU C++ compiler" \
        "pkg-config:pkg-config"; do
        cmd="${spec%%:*}"
        label="${spec#*:}"
        command -v "${cmd}" >/dev/null 2>&1 || missing+=("${label} (command: ${cmd})")
    done
    if ((do_lint)) && ! command -v git >/dev/null 2>&1; then
        missing+=("Git (command: git; required by --lint/--all)")
    fi

    if command -v pkg-config >/dev/null 2>&1; then
        for spec in \
            "vulkan:Vulkan headers and loader" \
            "xcb:core XCB" \
            "xcb-composite:XCB Composite" \
            "xcb-shape:XCB Shape" \
            "xcb-xtest:XCB XTest" \
            "xcb-xfixes:XCB XFixes" \
            "gl:OpenGL development files" \
            "x11:Xlib development files"; do
            module="${spec%%:*}"
            label="${spec#*:}"
            pkg-config --exists "${module}" || missing+=("${label} (pkg-config: ${module})")
        done
    fi

    if ((${#missing[@]} == 0)); then
        ((check_deps_only)) || ok "Linux/WSL build dependencies found"
        return 0
    fi

    printf '\nLinux/WSL dependency preflight FAILED. Missing:\n' >&2
    printf '  - %s\n' "${missing[@]}" >&2
    cat >&2 <<'EOF'

Install the complete Ubuntu build set inside WSL:
  sudo apt update
  sudo apt install build-essential cmake ninja-build pkg-config git \
      libvulkan-dev libxcb1-dev libxcb-composite0-dev libxcb-shape0-dev \
      libxcb-xtest0-dev libxcb-xfixes0-dev libgl-dev libx11-dev

Runtime/session packages (Weston, private Xwayland prerequisites, Mesa/Zink, and tools) are listed in:
  docs/building.md#wsl-dependencies
EOF
    return 1
}

# Runtime-readiness gate: rebuilding vkrelay2 does not compile Xwayland, but a successful Linux lane
# must leave the checkout able to launch applications. Reuse the launcher's exact admission helpers
# so preflight and app-run cannot disagree about an explicit server or checkout-local stage.
# shellcheck source=../../linux/launcher/lib_private_session.sh
. "${repo_root}/linux/launcher/lib_private_session.sh"

private_xwayland_preflight() {
    local override="${VKRELAY2_XWAYLAND_BIN:-}"
    if [[ -n "${override}" ]]; then
        if [[ -x "${override}" ]]; then
            ((check_deps_only)) || ok "Explicit tested Xwayland found (${override})"
            return 0
        fi
        printf '\nPrivate Xwayland preflight FAILED.\n' >&2
        printf '  - VKRELAY2_XWAYLAND_BIN is not executable: %s\n' "${override}" >&2
        printf 'Fix or unset the override, then retry.\n' >&2
        return 1
    fi

    local codename="" arch="" installed="" resolved=""
    [[ -r /etc/os-release ]] && codename="$(. /etc/os-release; printf '%s' "${VERSION_CODENAME:-}")"
    command -v dpkg >/dev/null 2>&1 && arch="$(dpkg --print-architecture 2>/dev/null || true)"
    command -v dpkg-query >/dev/null 2>&1 \
        && installed="$(dpkg-query -W -f='${Version}' xwayland 2>/dev/null || true)"

    # Match launcher policy: only the empirically known-unsafe distro/arch set requires the guarded
    # stage. An untested host may use its system server; --windows-only never calls this function.
    if ! vkr_stock_xwayland_known_unsafe "${codename}" "${arch}" "${installed}" ""; then
        ((check_deps_only)) || ok "Private Xwayland stage not required for ${codename:-unknown}/${arch:-unknown}"
        return 0
    fi

    if resolved="$(vkr_find_compatible_private_xwayland)"; then
        ((check_deps_only)) || ok "Compatible private Xwayland stage found (${resolved})"
        return 0
    fi

    local candidate="${repo_root}/build/src_ext/xwayland/stage/Xwayland"
    local provenance="${repo_root}/build/src_ext/xwayland/stage/PROVENANCE.txt"
    printf '\nPrivate Xwayland preflight FAILED. Missing requirement:\n' >&2
    printf '  - A compatible guarded Xwayland stage for %s/%s (installed package: %s).\n' \
        "${codename:-unknown}" "${arch:-unknown}" "${installed:-not installed}" >&2
    if [[ ! -x "${candidate}" ]]; then
        printf '    No executable stage exists at %s.\n' "${candidate}" >&2
    elif [[ ! -r "${provenance}" ]]; then
        printf '    The stage exists, but %s is missing or unreadable.\n' "${provenance}" >&2
    elif ! command -v readelf >/dev/null 2>&1; then
        printf '    The stage cannot be verified because readelf is unavailable (install binutils).\n' >&2
    else
        local stage_target stage_source stage_freshness
        stage_target="$(awk '$1 == "target:" { print $2; exit }' "${provenance}")"
        stage_source="$(awk '$1 == "source_package:" { print $2; exit }' "${provenance}")"
        stage_freshness="$(awk '$1 == "freshness_verified:" { print $2; exit }' "${provenance}")"
        printf '    Existing stage metadata: target=%s, source=%s, freshness=%s.\n' \
            "${stage_target:-unknown}" "${stage_source:-unknown}" "${stage_freshness:-unknown}" >&2
        printf '    Rebuild it because its host/package provenance or ELF build ID no longer matches.\n' >&2
    fi
    cat >&2 <<EOF

Build or refresh the one-time stage (this is separate from rebuild_all.sh):
  sudo apt update
  sudo apt install dpkg-dev quilt curl util-linux devscripts equivs xwayland
  VKRELAY2_XWL_INSTALL_DEPS=1 \\
      bash ${repo_root}/src_ext/xwayland/build_private_xwayland.sh

Binary-package users already receive this stage. To test another known-good server instead:
  VKRELAY2_XWAYLAND_BIN=/absolute/path/to/Xwayland ./scripts/dev/rebuild_all.sh

Additional detail: docs/building.md#when-the-private-build-is-needed
EOF
    return 1
}

visual_studio_install_guidance() {
    cat >&2 <<'EOF'

Install Visual Studio 2026 through Visual Studio Installer. It is normally detected under:
  C:\Program Files\Microsoft Visual Studio\18\<Edition>
For a custom install root, set VKRELAY2_VS_DIR to its Windows path.
EOF
    if ((do_windows)); then
        cat >&2 <<'EOF'
Select the "Desktop development with C++" workload and these individual components:
  - MSVC Build Tools for x64/x86 (Latest)
  - C++ CMake tools for Windows
  - Windows 11 SDK
EOF
    fi
    if ((do_lint)); then
        printf '%s\n' '  - C++ Clang tools for Windows (--lint/--all)' >&2
    fi
}

windows_dependency_preflight() {
    if ! have_windows_interop; then
        printf '\nWindows dependency preflight FAILED: cmd.exe/wslpath interop is unavailable.\n' >&2
        printf 'Run this script from WSL2, or select only the Linux lane with --linux-only.\n' >&2
        return 1
    fi
    if ! windows_interop_executes; then
        windows_interop_guidance
        return 1
    fi
    if [[ -z "${VS}" ]]; then
        printf '\nWindows dependency preflight FAILED: Visual Studio 2026 was not found.\n' >&2
        visual_studio_install_guidance
        return 1
    fi
    if ((do_lint)) && [[ -z "${VS_UNIX}" ]]; then
        printf '\nWindows dependency preflight FAILED: cannot resolve %s inside WSL.\n' "${VS}" >&2
        printf 'Set VKRELAY2_VS_DIR to the Visual Studio install root as a Windows path.\n' >&2
        return 1
    fi

    if run_windows probe "${windows_preset}" "${do_windows}" "${do_lint}"; then
        ((check_deps_only)) || ok "Windows/VS dependencies found (${VS})"
        return 0
    fi

    printf '\nWindows dependency preflight FAILED. Missing items are listed above.\n' >&2
    printf 'Additional detail: docs/building.md#windows-dependencies\n' >&2
    return 1
}

dependency_preflight() {
    local failed=0
    step "dependency preflight"
    if ((do_linux)); then
        linux_dependency_preflight || failed=1
        private_xwayland_preflight || failed=1
    fi
    if ((do_windows || do_lint)); then
        windows_dependency_preflight || failed=1
    fi
    ((failed == 0)) || die "dependency preflight failed; resolve the items above and retry"
}

dependency_preflight
if ((check_deps_only)); then
    step "DONE"
    ok "dependency preflight passed (no build requested)"
    exit 0
fi

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
    require_vs
    [[ -n "${VS_UNIX}" ]] || die "cannot resolve '${VS}' to a local path (needs wslpath or cygpath)"
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
