#!/usr/bin/env bash
# regression: the launcher routes OpenGL through zink BY DEFAULT (no --frontend flag needed), and
# `--frontend vulkan13` is the ONLY opt-out. Sources the launcher lib (pure function definitions, no
# side effects) and asserts the pure vkr_gl_frontend_env decision for representative flag sets. Fast
# (no session/display spin-up) and runs identically on both platforms via ctest.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
lib="${here}/../../linux/launcher/lib_private_session.sh"
if [ ! -f "${lib}" ]; then
    echo "GL-FRONTEND-ENV: SKIP (lib not found: ${lib})"
    exit 0
fi
# shellcheck source=/dev/null
. "${lib}"

if ! command -v vkr_gl_frontend_env >/dev/null 2>&1; then
    echo "GL-FRONTEND-ENV: FAIL (vkr_gl_frontend_env not defined after sourcing the lib)"
    exit 1
fi

fail=0

# The full GL env the default/zink path must export (matching the original relay's wrapper).
zink_keys="MESA_LOADER_DRIVER_OVERRIDE=zink __GLX_VENDOR_LIBRARY_NAME=mesa GALLIUM_DRIVER=zink LIBGL_KOPPER_DRI2=1"

expect_zink() { # <desc> <flags...>
    local desc="$1"
    shift
    local out
    out="$(vkr_gl_frontend_env "$@")"
    local missing=""
    local k
    for k in ${zink_keys}; do
        printf '%s\n' "${out}" | grep -qxF "${k}" || missing="${missing} ${k}"
    done
    if [ -z "${missing}" ]; then
        echo "ok: ${desc} -> zink env"
    else
        echo "FAIL: ${desc} -> missing[${missing} ] got[${out}]"
        fail=1
    fi
}

expect_empty() { # <desc> <flags...>
    local desc="$1"
    shift
    local out
    out="$(vkr_gl_frontend_env "$@")"
    if [ -z "${out}" ]; then
        echo "ok: ${desc} -> no GL env (opt-out)"
    else
        echo "FAIL: ${desc} -> expected empty, got[${out}]"
        fail=1
    fi
}

# Default + every non-opt-out form routes GL through zink (no flag needed -- the fix).
expect_zink "default (no flags)"
expect_zink "gpu-only" --gpu auto
expect_zink "explicit frontend auto" --frontend auto
expect_zink "explicit opengl46zink" --frontend opengl46zink
expect_zink "gpu + opengl46zink" --gpu auto --frontend opengl46zink
expect_zink "display flag (no frontend)" --display auto

# `--frontend vulkan13` is the only opt-out, even alongside other flags.
expect_empty "vulkan13 opt-out" --frontend vulkan13
expect_empty "gpu + vulkan13" --gpu auto --frontend vulkan13
expect_empty "vulkan13 + display" --frontend vulkan13 --display auto

# (native lane): vkr_api_version_env emits the marker AUTHORITATIVELY -- 1 for the native
# frontend (--frontend vulkan13), 0 for every GL lane. The 0 is an ACTIVE override (not "unset"),
# so a contaminated parent VKRELAY2_NATIVE_LANE can never uncap a zink run.
if ! command -v vkr_api_version_env >/dev/null 2>&1; then
    echo "GL-FRONTEND-ENV: FAIL (vkr_api_version_env not defined after sourcing the lib)"
    exit 1
fi
expect_lane() { # <desc> <mesa-major-pin> <expected 0|1> <flags...>
    local desc="$1" mesa="$2" want="$3"
    shift 3
    local out
    out="$(VKRELAY2_MESA_MAJOR="${mesa}" vkr_api_version_env "$@")"
    if [ "${out}" = "VKRELAY2_NATIVE_LANE=${want}" ]; then
        echo "ok: ${desc} -> lane ${want}"
    else
        echo "FAIL: ${desc} -> expected lane ${want}, got[${out}]"
        fail=1
    fi
}
# The native frontend is 1 regardless of Mesa version.
expect_lane "native (vulkan13), mesa 23" 23 1 --frontend vulkan13
expect_lane "native + gpu, mesa 25" 25 1 --gpu auto --frontend vulkan13
# GL lanes on Mesa <= 23 (the proven jammy configuration): the zink-safe 1.2 cap, unchanged.
expect_lane "default (no flags), mesa 23" 23 0
expect_lane "gpu-only, mesa 23" 23 0 --gpu auto
expect_lane "opengl46zink, mesa 23" 23 0 --frontend opengl46zink
expect_lane "frontend auto, mesa 23" 23 0 --frontend auto
# GL lanes on Mesa >= 24: newer zink REQUIRES a Vulkan 1.3 physical device (it silently rejects a
# 1.2 one at pdev selection -- the Ubuntu 24.04/26.04 finding), so the launcher opens the lane.
expect_lane "default (no flags), mesa 24" 24 1
expect_lane "frontend auto, mesa 25" 25 1 --frontend auto
expect_lane "opengl46zink, mesa 26" 26 1 --frontend opengl46zink
# Unknown Mesa (probe found nothing) stays the conservative legacy cap.
expect_lane "default, unknown mesa" "" 0

if [ "${fail}" -eq 0 ]; then
    echo "GL-FRONTEND-ENV: PASS"
    exit 0
fi
echo "GL-FRONTEND-ENV: FAIL"
exit 1
