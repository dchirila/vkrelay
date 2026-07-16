#include "common/protocol/gpu.hpp"
#include "tests/test_assert.hpp"

#include <string>

using namespace vkr::protocol;

namespace {

void test_parse_selectors() {
    VKR_CHECK(parse_selector("auto").ok);
    VKR_CHECK(parse_selector("auto").selector.kind == SelectorKind::Auto);
    VKR_CHECK(parse_selector("high-performance").selector.kind == SelectorKind::HighPerformance);
    VKR_CHECK(parse_selector("integrated").selector.kind == SelectorKind::Integrated);

    auto vendor = parse_selector("vendor:0x8086");
    VKR_CHECK(vendor.ok);
    VKR_CHECK(vendor.selector.kind == SelectorKind::Vendor);
    VKR_CHECK_EQ(vendor.selector.vendor_id, static_cast<std::uint32_t>(0x8086));

    auto device = parse_selector("device:0x1002:0x164e");
    VKR_CHECK(device.ok);
    VKR_CHECK_EQ(device.selector.vendor_id, static_cast<std::uint32_t>(0x1002));
    VKR_CHECK_EQ(device.selector.device_id, static_cast<std::uint32_t>(0x164e));

    auto idx = parse_selector("index:2");
    VKR_CHECK(idx.ok);
    VKR_CHECK_EQ(idx.selector.index, 2);
    VKR_CHECK(selector_is_unstable(idx.selector));

    VKR_CHECK(parse_selector("name:Intel").ok);
    VKR_CHECK(parse_selector("luid:0xabc").ok);

    // Invalid inputs are rejected with a message.
    VKR_CHECK(!parse_selector("vendor:nothex").ok);
    VKR_CHECK(!parse_selector("device:0x1").ok);
    VKR_CHECK(!parse_selector("bogus").ok);
    VKR_CHECK(!parse_selector("index:-1").ok);
}

void test_selection() {
    const auto devices = probe_mocked();
    VKR_CHECK(devices.size() >= 3);

    std::string reason;
    const GpuDevice* hp =
        select_device(devices, parse_selector("high-performance").selector, reason);
    VKR_CHECK(hp != nullptr);
    if (hp != nullptr) {
        VKR_CHECK(hp->type == GpuDeviceType::Discrete);
    }

    const GpuDevice* integ = select_device(devices, parse_selector("integrated").selector, reason);
    VKR_CHECK(integ != nullptr);
    if (integ != nullptr) {
        VKR_CHECK(integ->type == GpuDeviceType::Integrated);
    }

    const GpuDevice* intel =
        select_device(devices, parse_selector("vendor:0x8086").selector, reason);
    VKR_CHECK(intel != nullptr);
    if (intel != nullptr) {
        VKR_CHECK_EQ(intel->vendor_id, static_cast<std::uint32_t>(0x8086));
    }

    const GpuDevice* byname =
        select_device(devices, parse_selector("name:Radeon").selector, reason);
    VKR_CHECK(byname != nullptr);

    // Unmatched selector returns nullptr with a reason.
    const GpuDevice* none =
        select_device(devices, parse_selector("vendor:0xdead").selector, reason);
    VKR_CHECK(none == nullptr);
    VKR_CHECK(!reason.empty());
}

void test_format_non_empty() {
    const std::string text = format_gpu_list(probe_mocked());
    VKR_CHECK(text.find("usable=yes") != std::string::npos);
    VKR_CHECK(text.find("NVIDIA") != std::string::npos);
    VKR_CHECK(format_gpu_list({}).find("no Vulkan adapters") != std::string::npos);
}

} // namespace

int main() {
    test_parse_selectors();
    test_selection();
    test_format_non_empty();
    return vkr::test::finish("unit_gpu");
}
