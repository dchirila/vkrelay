#include "windows/worker/real_gpu_probe.hpp"

#include "common/logging/logging.hpp"
#include "common/vkrpc/vulkan_session.hpp" // kSupportedApiMajor / kSupportedApiMinor

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkr::worker {
namespace {

constexpr char kComponent[] = "gpu-probe";

protocol::GpuDeviceType map_device_type(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return protocol::GpuDeviceType::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return protocol::GpuDeviceType::Integrated;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return protocol::GpuDeviceType::Cpu;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return protocol::GpuDeviceType::Virtual;
    default:
        return protocol::GpuDeviceType::Other;
    }
}

std::string version_string(std::uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) +
           "." + std::to_string(VK_API_VERSION_PATCH(v));
}

// Loader instance-level version (1.0 if vkEnumerateInstanceVersion is absent).
std::uint32_t query_instance_version() {
    if (auto pfn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))) {
        std::uint32_t iv = 0;
        if (pfn(&iv) == VK_SUCCESS) {
            return iv;
        }
    }
    return VK_API_VERSION_1_0;
}

bool has_graphics_queue(VkPhysicalDevice phys) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return true;
        }
    }
    return false;
}

// Real driver name (1.2+) and LUID (1.1+) via Properties2, when the instance
// supports it. Both are best-effort: left untouched if unavailable.
void query_driver_and_luid(VkPhysicalDevice phys, std::uint32_t instance_version,
                           std::string& driver_name, std::string& luid) {
    if (instance_version < VK_API_VERSION_1_1) {
        return; // vkGetPhysicalDeviceProperties2 is core 1.1
    }
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    const bool have_driver_props = instance_version >= VK_API_VERSION_1_2;
    if (have_driver_props) {
        id.pNext = &driver; // VkPhysicalDeviceDriverProperties is core 1.2
    }
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &id;
    vkGetPhysicalDeviceProperties2(phys, &props2);

    if (have_driver_props && driver.driverName[0] != '\0') {
        driver_name = driver.driverName;
    }
    if (id.deviceLUIDValid) {
        luid = format_luid(id.deviceLUID, VK_LUID_SIZE);
    }
}

} // namespace

std::vector<protocol::GpuDevice> probe_real_gpus() {
    std::vector<protocol::GpuDevice> result;

    const std::uint32_t instance_version = query_instance_version();
    // Request min(loader version, worker ceiling) so a 1.0-1.2 host still works.
    std::uint32_t requested =
        VK_MAKE_API_VERSION(0, vkrpc::kSupportedApiMajor, vkrpc::kSupportedApiMinor, 0);
    if (instance_version < requested) {
        requested = instance_version;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = requested;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        VKR_WARN(kComponent) << "no usable Vulkan loader/driver";
        return result;
    }

    std::uint32_t count = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
        vkDestroyInstance(instance, nullptr);
        return result;
    }
    std::vector<VkPhysicalDevice> devs(count);
    r = vkEnumeratePhysicalDevices(instance, &count, devs.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        vkDestroyInstance(instance, nullptr);
        return result;
    }
    devs.resize(count); // VK_INCOMPLETE: only the first `count` handles are valid

    bool tagged_high_performance = false;
    bool tagged_integrated = false;
    for (std::size_t i = 0; i < devs.size(); ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devs[i], &props);

        protocol::GpuDevice d;
        d.index = static_cast<int>(i);
        d.name = props.deviceName;
        d.vendor_id = props.vendorID;
        d.device_id = props.deviceID;
        d.type = map_device_type(props.deviceType);
        d.api_version = version_string(props.apiVersion);
        query_driver_and_luid(devs[i], instance_version, d.driver_name, d.luid);

        // Usability: a device we could actually create a graphics device on. Role
        // detection (present/composite) needs WSI checks and is a later refinement;
        // a usable device carries the same provisional roles the mock reports.
        const bool usable = has_graphics_queue(devs[i]);
        d.usable = usable;
        d.reason = usable ? "ok" : "no graphics queue family";
        if (usable) {
            d.roles = {"top-level-present", "readback-composite"};
        }

        // Synthesize the default tags a selector would use: the first usable
        // discrete GPU is the high-performance default; the first usable
        // integrated GPU is the integrated default.
        if (usable && !tagged_high_performance && d.type == protocol::GpuDeviceType::Discrete) {
            d.default_high_performance = true;
            tagged_high_performance = true;
        } else if (usable && !tagged_integrated && d.type == protocol::GpuDeviceType::Integrated) {
            d.default_integrated = true;
            tagged_integrated = true;
        }

        result.push_back(std::move(d));
    }

    vkDestroyInstance(instance, nullptr);
    return result;
}

std::string format_luid(const std::uint8_t* luid, std::size_t n) {
    std::string hex = "0x";
    const char* digits = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned byte = luid[i];
        hex.push_back(digits[(byte >> 4) & 0xF]);
        hex.push_back(digits[byte & 0xF]);
    }
    return hex;
}

} // namespace vkr::worker
