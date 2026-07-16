#!/usr/bin/env bash
# vkrelay2 hostQueryReset smoke (Vulkan 1.3 required-feature audit): the DEVICE-level
# vkResetQueryPool, end to end on the REAL backend. hostQueryReset is REQUIRED since Vulkan 1.2 but
# the relay wired only the COMMAND-buffer vkCmdResetQueryPool; the device-level vkResetQueryPool (the
# feature itself) was unserved -- an advertise-then-fail gap this test closes.
#
# hostQueryReset is CORE 1.2, so it is served on BOTH lanes (it is not native-lane-gated like the
# 1.3-extension canaries). We therefore assert the canary PASSes on the native lane AND the default
# (zink) lane -- a required core feature, honestly served regardless of the reported version.
#
# REAL backend required (a real timestamp write + a real host-side reset); SKIPs cleanly (exit 0)
# when the relay cannot establish a real session (not a GPU box).
#
# Process guard: this smoke ECHOES the exact worker binary the launcher chose (path +
# flavor + mtime), so a stale-binary run cannot masquerade as green. Pin VKRELAY2_WIN_BUILD to force
# a specific build.
#
# Usage: bash run_vk13_hostquery_smoke.sh [<build-dir>]   (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-HOSTQUERY-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-hostquery-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

# Run the canary through the relay with the given launcher args; echo its lines to stderr (indented)
# and stdout (raw, for grepping) + surface the launched worker binary. SKIPs if not a GPU box.
run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    # Process guard: surface the FULL build provenance -- BOTH the linux launch/ICD
    # and the windows worker (path + flavor + mtime) + any STALE-build warning. A stale linux ICD or
    # a stale windows worker is exactly what made an earlier "green" run bogus (the abort-stub /
    # pre-change vouch), so make it impossible to miss.
    printf '%s\n' "${out}" |
        grep -E "build: (linux launch|windows worker) flavor=|WARNING.*(stale|OLDER)" |
        sed "s/^/    [${label}] /" >&2
    printf '%s\n' "${out}" | grep -E "VK13-HOSTQUERY-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-HOSTQUERY-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-HOSTQUERY-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# hostQueryReset is core 1.2 -- assert PASS on BOTH lanes (served regardless of reported version).
for lane in "native --frontend vulkan13" "default"; do
    label="${lane%% *}"
    args="${lane#"${label}"}"
    # shellcheck disable=SC2086
    out="$(run_canary "${label}" ${args})"
    if ! printf '%s\n' "${out}" | grep -q "VK13-HOSTQUERY-CANARY: PASS"; then
        echo "VK13-HOSTQUERY-SMOKE: FAIL (${label} lane: device-level vkResetQueryPool did not"
        echo "                     serve -- see the lines above)"
        exit 1
    fi
    if ! printf '%s\n' "${out}" | grep -qE "VK13-HOSTQUERY-CANARY: reset_proc_resolves=1"; then
        echo "VK13-HOSTQUERY-SMOKE: FAIL (${label} lane: vkResetQueryPool did not resolve)"
        exit 1
    fi
done

echo "============================================================"
echo "VK13-HOSTQUERY-SMOKE: PASS (device-level vkResetQueryPool served on both lanes: the proc"
echo "                     resolves and a HOST-side reset toggles a timestamp query's availability"
echo "                     available->unavailable on the real GPU -- hostQueryReset, a required 1.2"
echo "                     feature, is no longer advertise-then-failed)"
echo "============================================================"
exit 0
