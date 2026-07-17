#!/usr/bin/env bash
# Remove the per-Windows-user executable cache materialized by an installed vkrelay2 .deb.
# Invoked from the package prerm on remove/upgrade. It deletes only directories carrying the exact
# package marker, so an unexpected path or administrator-owned directory is never recursively removed.
set -euo pipefail

package_id="${1:-}"
case "${package_id}" in
    "" | *[!A-Za-z0-9.+~_-]*)
        echo "vkrelay2-prerm: refusing unsafe package id '${package_id}'" >&2
        exit 1
        ;;
esac

shopt -s nullglob
candidates=()
if [[ -n "${VKRELAY2_WINDOWS_CACHE_ROOT:-}" ]]; then
    candidates+=("${VKRELAY2_WINDOWS_CACHE_ROOT}/vkrelay2/${package_id}")
else
    # LOCALAPPDATA has this conventional location for normal Windows profiles. Cover every mounted
    # Windows drive/profile because dpkg runs as root and cannot ask which user first launched vkrun.
    candidates+=(/mnt/?/Users/*/AppData/Local/vkrelay2/"${package_id}")
fi

# dpkg runs maintainer scripts as root, whose secure PATH normally excludes Windows tools.
# Locate PowerShell explicitly and stop only processes loaded from this package version's cache.
powershell="$(command -v powershell.exe 2>/dev/null || true)"
if [[ -z "${powershell}" ]]; then
    for candidate in /mnt/?/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/System32/WindowsPowerShell/v1.0/powershell.exe; do
        if [[ -x "${candidate}" ]]; then
            powershell="${candidate}"
            break
        fi
    done
fi
if [[ -n "${powershell}" ]]; then
    "${powershell}" -NoProfile -NonInteractive -Command \
        "\$needle = '\\vkrelay2\\${package_id}\\'; Get-CimInstance Win32_Process | Where-Object { \$_.Name -in @('vkrelay2-supervisor.exe','vkrelay2-worker.exe') -and \$_.ExecutablePath -and \$_.ExecutablePath.IndexOf(\$needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }" \
        </dev/null >/dev/null 2>&1 || true
fi

failed=0
for cache in "${candidates[@]}"; do
    [[ -d "${cache}" ]] || continue
    marker="${cache}/.vkrelay2-package"
    if [[ ! -r "${marker}" || "$(tr -d '\r\n' < "${marker}")" != "${package_id}" ]]; then
        echo "vkrelay2-prerm: refusing unmarked Windows cache ${cache}" >&2
        failed=1
        continue
    fi
    echo "vkrelay2-prerm: removing Windows payload ${cache}" >&2
    # Process termination and Windows file-handle release are asynchronous. Retry briefly instead
    # of leaving dpkg in a broken half-removed state because a just-stopped log is still locked.
    # Keep the marker until every other entry is gone so a failed prerm can be retried safely.
    for attempt in {1..20}; do
        for entry in "${cache}"/* "${cache}"/.[!.]* "${cache}"/..?*; do
            [[ -e "${entry}" || -L "${entry}" ]] || continue
            [[ "${entry}" == "${marker}" ]] && continue
            rm -rf -- "${entry}" 2>/dev/null || true
        done
        remaining=0
        for entry in "${cache}"/* "${cache}"/.[!.]* "${cache}"/..?*; do
            [[ -e "${entry}" || -L "${entry}" ]] || continue
            [[ "${entry}" == "${marker}" ]] || remaining=1
        done
        if ((remaining == 0)); then
            rm -f -- "${marker}" 2>/dev/null || true
            rmdir "${cache}" 2>/dev/null || true
        fi
        [[ -e "${cache}" ]] || break
        sleep 0.1
    done
    if [[ -e "${cache}" ]]; then
        echo "vkrelay2-prerm: Windows payload still exists after removal: ${cache}" >&2
        failed=1
    else
        rmdir "$(dirname "${cache}")" 2>/dev/null || true
    fi
done

if ((failed)); then
    echo "vkrelay2-prerm: could not remove every marked Windows payload; close vkrelay2 processes and retry" >&2
    exit 1
fi
