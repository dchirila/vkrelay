#!/usr/bin/env bash
# Remove the per-Windows-user executable cache materialized by an installed vkrelay2 .deb.
# Invoked from the package prerm on remove/upgrade. Deletion requires an exact package ownership
# marker; the marker lives beside the directory and is removed only after rmdir succeeds.
set -euo pipefail

package_id="${1:-}"
case "${package_id}" in
    "" | *[!A-Za-z0-9.+~_-]*)
        echo "vkrelay2-prerm: refusing unsafe package id '${package_id}'" >&2
        exit 1
        ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cache_root_helper="${script_dir}/windows-cache-root"
root_record="${VKRELAY2_WINDOWS_CACHE_ROOT_RECORD:-/var/lib/vkrelay2/windows-cache-root}"
shopt -s nullglob
declare -a roots=() candidates=() owned_caches=()

add_root() {
    local root="${1%/}" existing
    [[ "${root}" == /* ]] || return 0
    for existing in "${roots[@]}"; do
        [[ "${existing}" != "${root}" ]] || return 0
    done
    roots+=("${root}")
}

[[ -z "${VKRELAY2_WINDOWS_CACHE_ROOT:-}" ]] || add_root "${VKRELAY2_WINDOWS_CACHE_ROOT}"
if [[ -r "${root_record}" ]]; then
    add_root "$(tr -d '\r\n' < "${root_record}")"
fi
if [[ -x "${cache_root_helper}" ]]; then
    discovered_root="$("${cache_root_helper}" 2>/dev/null || true)"
    [[ -z "${discovered_root}" ]] || add_root "${discovered_root}"
fi
# Last-resort compatibility for packages installed before cache-root recording existed and for
# removal while WSL interop is unavailable. The authoritative recorded/dynamic path above also
# covers redirected profiles and custom WSL automount roots.
for root in /mnt/?/Users/*/AppData/Local; do
    add_root "${root}"
done
if command -v findmnt >/dev/null 2>&1; then
    while IFS= read -r mount; do
        for root in "${mount}"/Users/*/AppData/Local; do
            add_root "${root}"
        done
    done < <(findmnt -rn -t 9p -o TARGET,OPTIONS \
        | awk '$0 ~ /aname=drvfs/ { print $1 }')
fi

for root in "${roots[@]}"; do
    cache="${root}/vkrelay2/${package_id}"
    duplicate=0
    for existing in "${candidates[@]}"; do
        [[ "${existing}" != "${cache}" ]] || duplicate=1
    done
    ((duplicate == 0)) || continue
    candidates+=("${cache}")
done

write_owner_marker() {
    local marker="$1" tmp="${1}.tmp.$$"
    printf '%s\n' "${package_id}" > "${tmp}" || return 1
    if ! mv -f "${tmp}" "${marker}"; then
        rm -f "${tmp}" 2>/dev/null || true
        return 1
    fi
}

safe_incomplete_cache() {
    local cache="$1" entry base
    [[ -d "${cache}" && ! -L "${cache}" ]] || return 1
    for entry in "${cache}"/* "${cache}"/.[!.]* "${cache}"/..?*; do
        [[ -e "${entry}" || -L "${entry}" ]] || continue
        base="${entry##*/}"
        case "${base}" in
            .install.lock|.vkrelay2-ready|SHA256SUMS|vkrelay2-supervisor.exe|vkrelay2-worker.exe|\
            vkrelay2-worker-daemon.log|vkrelay2-worker-daemon.log.out|\
            .SHA256SUMS.tmp.*|.vkrelay2-ready.tmp.*|.vkrelay2-package.tmp.*|\
            .vkrelay2-supervisor.exe.tmp.*|.vkrelay2-worker.exe.tmp.*) ;;
            *) return 1 ;;
        esac
    done
    return 0
}

failed=0
policy_failed=0
cache_delete_failed=0
for cache in "${candidates[@]}"; do
    owner_marker="${cache}.vkrelay2-package"
    legacy_marker="${cache}/.vkrelay2-package"

    if [[ -e "${cache}" && ( ! -d "${cache}" || -L "${cache}" ) ]]; then
        echo "vkrelay2-prerm: refusing unexpected Windows cache path ${cache}" >&2
        failed=1
        policy_failed=1
        continue
    fi

    if [[ -e "${owner_marker}" ]]; then
        if [[ ! -r "${owner_marker}" \
              || "$(tr -d '\r\n' < "${owner_marker}")" != "${package_id}" ]]; then
            echo "vkrelay2-prerm: refusing invalid Windows cache marker ${owner_marker}" >&2
            failed=1
            policy_failed=1
            continue
        fi
    elif [[ -d "${cache}" && ! -L "${cache}" ]]; then
        if [[ -e "${legacy_marker}" ]]; then
            if [[ ! -r "${legacy_marker}" \
                  || "$(tr -d '\r\n' < "${legacy_marker}")" != "${package_id}" ]]; then
                echo "vkrelay2-prerm: refusing invalid legacy marker ${legacy_marker}" >&2
                failed=1
                policy_failed=1
                continue
            fi
            write_owner_marker "${owner_marker}" || { failed=1; continue; }
        elif safe_incomplete_cache "${cache}"; then
            echo "vkrelay2-prerm: removing incomplete Windows payload ${cache}" >&2
            write_owner_marker "${owner_marker}" || { failed=1; continue; }
        else
            echo "vkrelay2-prerm: refusing unmarked Windows cache with unexpected contents ${cache}" >&2
            failed=1
            policy_failed=1
            continue
        fi
    else
        # A launch can be interrupted after publishing ownership but before mkdir. Removing this
        # marker-only state is safe and prevents a permanent apt-remove trap.
        [[ ! -e "${owner_marker}" ]] || rm -f -- "${owner_marker}" || failed=1
        continue
    fi
    owned_caches+=("${cache}")
done

# Windows locks running executables. Stop only processes loaded from this exact package version.
# A failed interop call is not itself authoritative: verified deletion below decides success.
stop_state="unavailable"
powershell=""
if [[ -x "${cache_root_helper}" ]]; then
    powershell="$("${cache_root_helper}" --powershell 2>/dev/null || true)"
fi
if [[ -n "${powershell}" ]]; then
    if "${powershell}" -NoProfile -NonInteractive -Command \
        "\$needle = '\\vkrelay2\\${package_id}\\'; Get-CimInstance Win32_Process | Where-Object { \$_.Name -in @('vkrelay2-supervisor.exe','vkrelay2-worker.exe') -and \$_.ExecutablePath -and \$_.ExecutablePath.IndexOf(\$needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }" \
        </dev/null >/dev/null 2>&1; then
        stop_state="ok"
    else
        stop_state="failed"
    fi
fi

for cache in "${owned_caches[@]}"; do
    owner_marker="${cache}.vkrelay2-package"
    echo "vkrelay2-prerm: removing Windows payload ${cache}" >&2
    # Windows handle release is asynchronous. Keep the sibling ownership marker throughout every
    # retry; only a successful rmdir permits deleting it.
    for attempt in {1..50}; do
        for entry in "${cache}"/* "${cache}"/.[!.]* "${cache}"/..?*; do
            [[ -e "${entry}" || -L "${entry}" ]] || continue
            rm -rf -- "${entry}" 2>/dev/null || true
        done
        rmdir "${cache}" 2>/dev/null || true
        [[ -e "${cache}" ]] || break
        sleep 0.1
    done
    if [[ -e "${cache}" ]]; then
        echo "vkrelay2-prerm: Windows payload still exists after removal: ${cache}" >&2
        failed=1
        cache_delete_failed=1
    else
        rm -f -- "${owner_marker}" || failed=1
        rmdir "$(dirname "${cache}")" 2>/dev/null || true
    fi
done

if ((failed)); then
    if ((policy_failed)); then
        echo "vkrelay2-prerm: an unsafe/unrecognized cache was left untouched; inspect the path above" >&2
    elif ((cache_delete_failed)) && [[ "${stop_state}" != "ok" ]]; then
        echo "vkrelay2-prerm: Windows interop could not stop every cached vkrelay2 process." >&2
        echo "  From Windows, run 'wsl --shutdown'; reopen this distribution; then retry:" >&2
        echo "    sudo apt remove vkrelay2" >&2
    elif ((cache_delete_failed)); then
        echo "vkrelay2-prerm: close any remaining vkrelay2 process and retry 'sudo apt remove vkrelay2'" >&2
    else
        echo "vkrelay2-prerm: cleanup could not be completed; inspect the error above and retry" >&2
    fi
    exit 1
fi

rm -f -- "${root_record}" 2>/dev/null || true
rmdir "$(dirname "${root_record}")" 2>/dev/null || true
