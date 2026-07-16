#include "common/protocol/gpu.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace vkr::protocol {
namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string hex(std::uint32_t value, int width) {
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "0x%0*x", width, value);
    return buf.data();
}

bool parse_hex_u32(const std::string& text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    std::string body = text;
    if (body.size() > 2 && (body[0] == '0') && (body[1] == 'x' || body[1] == 'X')) {
        body = body.substr(2);
    }
    if (body.empty() || body.size() > 8) {
        return false;
    }
    std::uint32_t value = 0;
    for (char c : body) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    out = value;
    return true;
}

std::string join(const std::vector<std::string>& items, char sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out.push_back(sep);
        }
        out += items[i];
    }
    return out;
}

} // namespace

const char* to_string(GpuDeviceType type) {
    switch (type) {
    case GpuDeviceType::Discrete:
        return "discrete";
    case GpuDeviceType::Integrated:
        return "integrated";
    case GpuDeviceType::Cpu:
        return "cpu";
    case GpuDeviceType::Virtual:
        return "virtual";
    case GpuDeviceType::Other:
        return "other";
    }
    return "other";
}

std::vector<GpuDevice> probe_mocked() {
    std::vector<GpuDevice> devices;

    GpuDevice nvidia;
    nvidia.index = 0;
    nvidia.name = "NVIDIA T1200 Laptop GPU";
    nvidia.vendor_id = 0x10de;
    nvidia.device_id = 0x1f99;
    nvidia.type = GpuDeviceType::Discrete;
    nvidia.api_version = "1.3.280";
    nvidia.driver_name = "NVIDIA proprietary (mock)";
    nvidia.luid = "0x00000000000a1b2c";
    nvidia.usable = true;
    nvidia.reason = "ok";
    nvidia.roles = {"top-level-present", "readback-composite"};
    nvidia.default_high_performance = true;
    devices.push_back(nvidia);

    GpuDevice intel;
    intel.index = 1;
    intel.name = "Intel(R) UHD Graphics";
    intel.vendor_id = 0x8086;
    intel.device_id = 0x9a60;
    intel.type = GpuDeviceType::Integrated;
    intel.api_version = "1.3.280";
    intel.driver_name = "Intel (mock)";
    intel.luid = "0x00000000000a1b2d";
    intel.usable = true;
    intel.reason = "ok";
    intel.roles = {"top-level-present", "readback-composite"};
    intel.default_integrated = true;
    devices.push_back(intel);

    GpuDevice amd;
    amd.index = 2;
    amd.name = "AMD Radeon 610M";
    amd.vendor_id = 0x1002;
    amd.device_id = 0x164e;
    amd.type = GpuDeviceType::Integrated;
    amd.api_version = "1.3.280";
    amd.driver_name = "AMD (mock)";
    amd.luid = "0x00000000000a1b2e";
    amd.usable = true;
    amd.reason = "ok";
    amd.roles = {"top-level-present", "readback-composite"};
    devices.push_back(amd);

    return devices;
}

std::string format_gpu_list(const std::vector<GpuDevice>& devices) {
    std::ostringstream out;
    if (devices.empty()) {
        return "(no Vulkan adapters reported)\n";
    }
    for (const auto& d : devices) {
        std::string def = "none";
        if (d.default_high_performance) {
            def = "high-performance";
        } else if (d.default_integrated) {
            def = "integrated";
        }
        out << "[" << d.index << "] usable=" << (d.usable ? "yes" : "no") << " default=" << def
            << " type=" << to_string(d.type) << "\n";
        out << "    name=\"" << d.name << "\" vendor=" << hex(d.vendor_id, 4)
            << " device=" << hex(d.device_id, 4) << "\n";
        out << "    api=" << d.api_version << " driver=\"" << d.driver_name << "\" luid=" << d.luid
            << "\n";
        if (!d.usable) {
            out << "    reason=" << d.reason << "\n";
        }
        out << "    roles=" << join(d.roles, ',') << "\n";
    }
    return out.str();
}

SelectorParse parse_selector(const std::string& text) {
    SelectorParse result;
    result.selector.raw = text;
    const std::string lower = to_lower(text);

    auto fail = [&](const std::string& msg) {
        result.ok = false;
        result.error = msg;
        return result;
    };

    if (lower == "auto") {
        result.selector.kind = SelectorKind::Auto;
    } else if (lower == "high-performance") {
        result.selector.kind = SelectorKind::HighPerformance;
    } else if (lower == "integrated") {
        result.selector.kind = SelectorKind::Integrated;
    } else if (lower.rfind("vendor:", 0) == 0) {
        result.selector.kind = SelectorKind::Vendor;
        if (!parse_hex_u32(text.substr(7), result.selector.vendor_id)) {
            return fail("vendor selector requires a hex vendor id, e.g. vendor:0x8086");
        }
    } else if (lower.rfind("device:", 0) == 0) {
        result.selector.kind = SelectorKind::Device;
        const std::string body = text.substr(7);
        const auto colon = body.find(':');
        if (colon == std::string::npos ||
            !parse_hex_u32(body.substr(0, colon), result.selector.vendor_id) ||
            !parse_hex_u32(body.substr(colon + 1), result.selector.device_id)) {
            return fail("device selector requires hex vendor:device, e.g. device:0x1002:0x164e");
        }
    } else if (lower.rfind("luid:", 0) == 0) {
        result.selector.kind = SelectorKind::Luid;
        result.selector.luid = text.substr(5);
        if (result.selector.luid.empty()) {
            return fail("luid selector requires a value");
        }
    } else if (lower.rfind("index:", 0) == 0) {
        result.selector.kind = SelectorKind::Index;
        const std::string body = text.substr(6);
        if (body.empty() || body.find_first_not_of("0123456789") != std::string::npos) {
            return fail("index selector requires a non-negative integer");
        }
        result.selector.index = std::stoi(body);
    } else if (lower.rfind("name:", 0) == 0) {
        result.selector.kind = SelectorKind::Name;
        result.selector.name = text.substr(5);
        if (result.selector.name.empty()) {
            return fail("name selector requires a substring");
        }
    } else {
        return fail("unknown GPU selector '" + text + "'");
    }

    result.ok = true;
    return result;
}

bool selector_is_unstable(const GpuSelector& selector) {
    return selector.kind == SelectorKind::Index;
}

const GpuDevice* select_device(const std::vector<GpuDevice>& devices, const GpuSelector& selector,
                               std::string& reason) {
    auto first_usable = [&](auto pred) -> const GpuDevice* {
        for (const auto& d : devices) {
            if (d.usable && pred(d)) {
                return &d;
            }
        }
        return nullptr;
    };

    const GpuDevice* chosen = nullptr;
    switch (selector.kind) {
    case SelectorKind::Auto:
        chosen = first_usable([](const GpuDevice& d) { return d.default_high_performance; });
        if (chosen == nullptr) {
            chosen = first_usable([](const GpuDevice&) { return true; });
        }
        break;
    case SelectorKind::HighPerformance:
        chosen = first_usable([](const GpuDevice& d) { return d.default_high_performance; });
        if (chosen == nullptr) {
            chosen =
                first_usable([](const GpuDevice& d) { return d.type == GpuDeviceType::Discrete; });
        }
        break;
    case SelectorKind::Integrated:
        chosen =
            first_usable([](const GpuDevice& d) { return d.type == GpuDeviceType::Integrated; });
        break;
    case SelectorKind::Vendor:
        chosen =
            first_usable([&](const GpuDevice& d) { return d.vendor_id == selector.vendor_id; });
        break;
    case SelectorKind::Device:
        chosen = first_usable([&](const GpuDevice& d) {
            return d.vendor_id == selector.vendor_id && d.device_id == selector.device_id;
        });
        break;
    case SelectorKind::Luid:
        chosen = first_usable(
            [&](const GpuDevice& d) { return to_lower(d.luid) == to_lower(selector.luid); });
        break;
    case SelectorKind::Index:
        for (const auto& d : devices) {
            if (d.index == selector.index) {
                if (!d.usable) {
                    reason = "device at index " + std::to_string(selector.index) +
                             " is not usable: " + d.reason;
                    return nullptr;
                }
                chosen = &d;
                break;
            }
        }
        break;
    case SelectorKind::Name: {
        const std::string needle = to_lower(selector.name);
        chosen = first_usable(
            [&](const GpuDevice& d) { return to_lower(d.name).find(needle) != std::string::npos; });
        break;
    }
    }

    if (chosen == nullptr) {
        reason = "no usable adapter matched selector '" + selector.raw + "'";
        return nullptr;
    }
    reason = "selected " + chosen->name + " (index " + std::to_string(chosen->index) + ")";
    return chosen;
}

} // namespace vkr::protocol
