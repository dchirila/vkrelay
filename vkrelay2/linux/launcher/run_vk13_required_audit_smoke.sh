#!/usr/bin/env bash
# vkrelay2 required-feature AUDIT smoke (required-feature audit): the machine-checked
# conformance invariant behind the honest-1.3 claim -- the relay must never report apiVersion >= 1.3
# while a REQUIRED feature (multiview) is advertise-then-failed.
#
# The audit canary reports the f10/f11/f12/f13 required matrix, probes whether a viewMask render pass
# 2 is actually served, and asserts: multiview UNSERVED => the device honestly stays below 1.3. It
# PASSES today (native lane 1.2, multiview PENDING) and after (1.3, multiview SERVED); it
# FAILS only on a real advertise-then-fail regression. Run on BOTH lanes -- the invariant must hold
# on the native (1.3-capable) lane AND the default (zink 1.2) lane.
#
# REAL backend required (a real device + the viewMask probe); SKIPs cleanly otherwise.
#
# Process guard: echoes the exact worker/launch binaries + any stale-build warning.
#
# Usage: bash run_vk13_required_audit_smoke.sh [<build-dir>]  (default build-dir: ../../build/linux-debug)
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-${script_dir}/../../build/linux-debug}"

skip() { echo "VK13-AUDIT-SMOKE: SKIP ($1)"; exit 0; }

command -v weston >/dev/null 2>&1 || skip "weston not found (need WSL private-display tools)"
command -v Xwayland >/dev/null 2>&1 || skip "Xwayland not found"
canary="${build_dir}/vkrelay2-vk13-required-audit-canary"
[ -x "${canary}" ] || skip "missing binary ${canary} (build the linux preset first)"

run_canary() { # <label> <extra run_through args...>
    local label="$1"
    shift
    local out
    out="$(timeout 120 "${script_dir}/vkrun" "$@" -- "${canary}" 2>&1)"
    printf '%s\n' "${out}" |
        grep -E "build: (linux launch|windows worker) flavor=|WARNING.*(stale|OLDER)" |
        sed "s/^/    [${label}] /" >&2
    printf '%s\n' "${out}" | grep -E "VK13-AUDIT-CANARY:" | sed "s/^/    [${label}] /" >&2
    if ! printf '%s\n' "${out}" | grep -q "VK13-AUDIT-CANARY:"; then
        if printf '%s\n' "${out}" | grep -qE "no daemon reachable and no Windows build found"; then
            skip "no real daemon / no Windows build (not a GPU box)"
        fi
        echo "VK13-AUDIT-SMOKE: FAIL (${label}: bring-up did not reach the canary)"
        exit 1
    fi
    printf '%s\n' "${out}"
}

# The invariant must hold on BOTH lanes.
for lane in "native --frontend vulkan13" "default"; do
    label="${lane%% *}"
    args="${lane#"${label}"}"
    # shellcheck disable=SC2086
    out="$(run_canary "${label}" ${args})"
    if ! printf '%s\n' "${out}" | grep -q "VK13-AUDIT-CANARY: PASS"; then
        echo "VK13-AUDIT-SMOKE: FAIL (${label} lane: the required-feature audit invariant failed --"
        echo "                  a required feature is advertise-then-failed at the reported version)"
        exit 1
    fi
done

echo "============================================================"
echo "VK13-AUDIT-SMOKE: PASS (the required-feature audit invariant holds on both lanes: the relay"
echo "                  never reports apiVersion >= 1.3 while multiview -- a required feature -- is"
echo "                  advertise-then-failed; the served required matrix is honestly backed)"
echo "============================================================"
exit 0
