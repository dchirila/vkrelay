#!/usr/bin/env bash
# vkrelay2: run a Khronos Vulkan CTS (VK-GL-CTS deqp-vk) case subset THROUGH the relay.
#
# The CTS is the independent conformance oracle beyond our own canaries -- it runs as a normal
# Vulkan app on the NATIVE lane (the vkrelay2 ICD -> worker -> real GPU) and pressure-tests the
# honest-1.3 claim. It surfaced (and we fixed) real ICD bugs the single-enumerate canaries never
# could: non-persistent physical-device handles, a sparse-feature advertise-then-fail, and missing
# promoted standalone feature/property structs (see the 2026-07-08 vk13 audit session doc).
#
# deqp-vk is large + slow to build, so it is NOT vendored. Build it once in WSL (Ubuntu), then point
# this script at it via VKRELAY2_DEQP_VK (or the default path below):
#   git clone --depth 1 https://github.com/KhronosGroup/VK-GL-CTS.git ~/vkcts && cd ~/vkcts
#   python3 external/fetch_sources.py
#   cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=default && ninja -C build deqp-vk
#   # binary: build/external/vulkancts/modules/vulkan/deqp-vk  (needs X11/xcb/wayland/EGL dev headers)
#
# The relay tests render OFFSCREEN (--deqp-surface-type=fbo) on a headless private display -- nothing
# appears on the desktop; only console + the .qpa log.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
run_through="${script_dir}/../../linux/launcher/vkrun"
deqp="${VKRELAY2_DEQP_VK:-${HOME}/vkcts/build/external/vulkancts/modules/vulkan/deqp-vk}"

usage() {
  cat >&2 <<USAGE
Usage: bash run_vk_cts.sh [--allow-failures] "<deqp-case-glob>" <tag> [gpu-selector]

  --allow-failures  OPTIONAL survey mode: exit 0 even if cases Fail/Crash (default = exit 1 on any
                    Fail or Error/Crash, so the script can be wired into a gate). NotSupported never fails.
  <deqp-case-glob>  a deqp-vk --deqp-case pattern -- QUOTE it (the * is a deqp glob, not a shell one)
  <tag>             short label for the outputs: /tmp/vkcts_<tag>.qpa and /tmp/vkcts_<tag>.stdout
  [gpu-selector]    OPTIONAL adapter (passed to run_through --gpu). Omit = auto/discrete.
                    e.g. integrated | high-performance | vendor:0x1002 | name:AMD | index:0

  This script is invoked via bash (committed mode 100644, like most repo launchers -- no exec bit).
  Override the deqp-vk binary with VKRELAY2_DEQP_VK=/path/to/deqp-vk.

Examples:
  bash run_vk_cts.sh "dEQP-VK.api.info.vulkan1p3.*" vk1p3          # official 1.3 info conformance (5/5)
  bash run_vk_cts.sh "dEQP-VK.api.info.vulkan1p2.*" vk1p2         # 5/5
  bash run_vk_cts.sh --allow-failures "dEQP-VK.api.smoke.*" smoke amd integrated   # survey, never fails
List cases: "\$deqp" --deqp-runmode=stdout-caselist | less
USAGE
  exit 2
}
allow_failures=0
[ "${1:-}" = "--allow-failures" ] && { allow_failures=1; shift; }
[ "$#" -ge 2 ] || usage
[ -x "${deqp}" ] || { echo "run_vk_cts: deqp-vk not found at ${deqp} (build it, or set VKRELAY2_DEQP_VK) -- SKIP"; exit 0; }

case_glob="$1"; tag="$2"; gpu="${3:-}"
vkdir="$(dirname "${deqp}")"
qpa="/tmp/vkcts_${tag}.qpa"; log="/tmp/vkcts_${tag}.stdout"; wrapper="/tmp/vkcts_exec_${tag}.sh"
rm -f "${qpa}" "${log}"
gpu_args=()
[ -n "${gpu}" ] && gpu_args=(--gpu "${gpu}")

# Hardcode the deqp invocation into a wrapper: nested quotes do NOT survive vkrun
# re-exec of its "-- <app>" command, so pass a plain "bash <wrapper>".
cat > "${wrapper}" <<INNER
#!/usr/bin/env bash
cd "${vkdir}" || exit 97
exec ./deqp-vk --deqp-case="${case_glob}" --deqp-surface-type=fbo \
  --deqp-log-images=disable --deqp-log-shader-sources=disable --deqp-log-filename="${qpa}"
INNER
chmod +x "${wrapper}"

timeout 1800 bash "${run_through}" "${gpu_args[@]}" --frontend vulkan13 -- bash "${wrapper}" \
  > "${log}" 2>&1

echo "==== [${tag}] result ===="
if [ -f "${qpa}" ]; then
  dev="$(grep -ihoE "AMD Radeon[^<\"]*|NVIDIA[^<\"]*|Intel[^<\"]*" "${qpa}" | head -1)"
  p=$(grep -c "StatusCode=\"Pass\"" "${qpa}"); f=$(grep -c "StatusCode=\"Fail\"" "${qpa}")
  ns=$(grep -c "StatusCode=\"NotSupported\"" "${qpa}"); qw=$(grep -c "StatusCode=\"QualityWarning\"" "${qpa}")
  ce=$(grep -cE "StatusCode=\"(InternalError|Crash|ResourceError|Timeout)\"" "${qpa}")
  echo "adapter: ${dev:-unknown}"
  echo "Pass=${p} Fail=${f} NotSupported=${ns} QualityWarning=${qw} Error/Crash=${ce}"
  awk "/#beginTestCaseResult/{c=\$2} /StatusCode=\"Fail\"/{print \"  FAIL \"c}" "${qpa}" | head -30
  if [ "${allow_failures}" -eq 0 ] && { [ "${f}" -gt 0 ] || [ "${ce}" -gt 0 ]; }; then
    echo "run_vk_cts: ${f} Fail + ${ce} Error/Crash -- FAIL (pass --allow-failures for survey runs)"
    exit 1
  fi
else
  echo "NO QPA LOG (relay bring-up or deqp init failed). stdout tail:"; tail -15 "${log}"
  exit 1
fi
