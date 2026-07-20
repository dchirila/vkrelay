#!/usr/bin/env bash
# Print the WSL path for the invoking Windows user's LocalAppData directory.
# dpkg maintainer scripts run with a root secure_path that normally omits Windows tools, so locate
# PowerShell explicitly instead of assuming powershell.exe is on PATH.
set -euo pipefail

powershell="$(command -v powershell.exe 2>/dev/null || true)"
if [[ -z "${powershell}" ]]; then
    shopt -s nullglob
    for candidate in /mnt/?/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/System32/WindowsPowerShell/v1.0/powershell.exe; do
        if [[ -x "${candidate}" ]]; then
            powershell="${candidate}"
            break
        fi
    done
fi
if [[ -z "${powershell}" ]] && command -v findmnt >/dev/null 2>&1; then
    while IFS= read -r mount; do
        candidate="${mount}/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
        if [[ -x "${candidate}" ]]; then
            powershell="${candidate}"
            break
        fi
    done < <(findmnt -rn -t 9p -o TARGET,OPTIONS \
        | awk '$0 ~ /aname=drvfs/ { print $1 }')
fi
[[ -n "${powershell}" ]] || exit 1
if [[ "${1:-}" == "--powershell" ]]; then
    printf '%s\n' "${powershell}"
    exit 0
fi
[[ $# -eq 0 ]] || exit 2
command -v wslpath >/dev/null 2>&1 || exit 1

win_local="$("${powershell}" -NoProfile -NonInteractive -Command \
    '[Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)' \
    </dev/null 2>/dev/null | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
[[ -n "${win_local}" ]] || exit 1
cache_root="$(wslpath -u "${win_local}" 2>/dev/null)"
[[ "${cache_root}" == /* ]] || exit 1
printf '%s\n' "${cache_root}"
