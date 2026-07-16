// Launch-time worker argv: proves the supervisor builds the worker command line that carries the
// faithful GPU-selection contract -- the resolved device's stable LUID as the match key (only when
// the list is real), the display name, and the --vulkan-backend pass-through. This guards the
// "all success markers, no real window" / "served the wrong GPU" regressions without spawning a
// worker (build_worker_argv is the single argv source launch_session uses). Dual-platform.
#include "common/protocol/gpu.hpp"
#include "tests/test_assert.hpp"
#include "windows/supervisor/worker_supervisor.hpp"

#include <cstddef>
#include <string>
#include <vector>

using vkr::protocol::GpuDevice;
using vkr::supervisor::build_worker_argv;

namespace {

// The value following `flag` in argv, or "" if `flag` is absent.
std::string value_after(const std::vector<std::string>& argv, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == flag) {
            return argv[i + 1];
        }
    }
    return "";
}

bool has_flag(const std::vector<std::string>& argv, const std::string& flag) {
    for (const auto& a : argv) {
        if (a == flag) {
            return true;
        }
    }
    return false;
}

GpuDevice make_device(const std::string& name, const std::string& luid) {
    GpuDevice d;
    d.name = name;
    d.luid = luid;
    return d;
}

// A real list resolves a concrete device: the launch is marked --gpu-required (faithful,
// fail-closed at the worker) and carries the stable LUID as the preferred match key, the display
// name, and
// --vulkan-backend (from the daemon's extra args). This is the contract the boundary canary depends
// on -- no silent mock, no silent substitution.
void test_real_passes_luid_and_backend() {
    const GpuDevice dev = make_device("NVIDIA GeForce RTX 4080 Laptop GPU", "0xb82f010000000000");
    const auto argv =
        build_worker_argv("vkrelay2-worker", /*worker_control_port=*/13579, "wkr-1", "tok-1", dev,
                          /*devices_real=*/true, /*worker_interval_ms=*/50, /*heartbeat_count=*/0,
                          /*data_plane=*/true, "app-token-xyz", /*sidecar_plane=*/false,
                          /*sidecar_token=*/"", {"--vulkan-backend", "real"});

    VKR_CHECK(has_flag(argv, "--gpu-required"));
    VKR_CHECK_EQ(value_after(argv, "--gpu"), dev.name);
    VKR_CHECK_EQ(value_after(argv, "--gpu-luid"), dev.luid);
    VKR_CHECK_EQ(value_after(argv, "--vulkan-backend"), std::string("real"));
    VKR_CHECK(has_flag(argv, "--data-plane"));
    VKR_CHECK_EQ(value_after(argv, "--app-token"), std::string("app-token-xyz"));
    VKR_CHECK_EQ(value_after(argv, "--worker-id"), std::string("wkr-1"));
    // No sidecar plane requested -> no sidecar args (the default until the launcher starts one).
    VKR_CHECK(!has_flag(argv, "--sidecar-plane"));
    VKR_CHECK(!has_flag(argv, "--sidecar-token"));
}

// A mock/auto list (devices_real=false) marks neither --gpu-required nor --gpu-luid: the worker
// stays lenient (discrete/first). The name is still forwarded for display. So a fabricated mock GPU
// never becomes a fail-closed mismatch -- the mock world is explicitly lenient.
void test_mock_is_lenient() {
    const GpuDevice dev = make_device("NVIDIA T1200 Laptop GPU", "0x00000000000a1b2c");
    const auto argv = build_worker_argv("vkrelay2-worker", 13579, "wkr-1", "tok-1", dev,
                                        /*devices_real=*/false, 50, 0, /*data_plane=*/true, "tok",
                                        /*sidecar_plane=*/false, /*sidecar_token=*/"",
                                        {"--vulkan-backend", "real"});
    VKR_CHECK(!has_flag(argv, "--gpu-required"));
    VKR_CHECK(!has_flag(argv, "--gpu-luid"));
    VKR_CHECK_EQ(value_after(argv, "--gpu"), dev.name);
}

// A real list whose device carries no LUID is STILL faithful: --gpu-required is set (so the worker
// fails closed on the exact name) even though no --gpu-luid is available. This is the no-LUID edge
// -- it must NOT degrade to lenient selection. data_plane=false
// also omits the app-reconnect args.
void test_real_without_luid_still_required() {
    const GpuDevice dev = make_device("Some GPU", /*luid=*/"");
    const auto argv = build_worker_argv("vkrelay2-worker", 13579, "wkr-1", "tok-1", dev,
                                        /*devices_real=*/true, 50, 0, /*data_plane=*/false, "",
                                        /*sidecar_plane=*/false, /*sidecar_token=*/"", {});
    VKR_CHECK(has_flag(argv, "--gpu-required")); // faithful, even without a LUID
    VKR_CHECK(!has_flag(argv, "--gpu-luid"));    // none available -> exact-name key
    VKR_CHECK_EQ(value_after(argv, "--gpu"), dev.name);
    VKR_CHECK(!has_flag(argv, "--data-plane"));
    VKR_CHECK(!has_flag(argv, "--app-token"));
}

// Sidecar plane: when a launch carries a sidecar plane, the worker argv gets
// --sidecar-plane + --sidecar-token, alongside (independent of) the app data-plane args.
void test_sidecar_plane_adds_token() {
    const GpuDevice dev = make_device("Some GPU", /*luid=*/"");
    const auto argv = build_worker_argv("vkrelay2-worker", 13579, "wkr-1", "tok-1", dev,
                                        /*devices_real=*/false, 50, 0, /*data_plane=*/true, "app-t",
                                        /*sidecar_plane=*/true, /*sidecar_token=*/"side-xyz", {});
    VKR_CHECK(has_flag(argv, "--data-plane"));
    VKR_CHECK_EQ(value_after(argv, "--app-token"), std::string("app-t"));
    VKR_CHECK(has_flag(argv, "--sidecar-plane"));
    VKR_CHECK_EQ(value_after(argv, "--sidecar-token"), std::string("side-xyz"));
}

} // namespace

int main() {
    test_real_passes_luid_and_backend();
    test_mock_is_lenient();
    test_real_without_luid_still_required();
    test_sidecar_plane_adds_token();
    return vkr::test::finish("unit_launch_argv");
}
