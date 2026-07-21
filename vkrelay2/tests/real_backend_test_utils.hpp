#ifndef VKRELAY2_TESTS_REAL_BACKEND_TEST_UTILS_HPP
#define VKRELAY2_TESTS_REAL_BACKEND_TEST_UTILS_HPP

#include "common/vkrpc/vulkan_session.hpp"
#include "windows/worker/real_vulkan_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace vkr::test {

// Select a compatible DEVICE_LOCAL type or a HOST_VISIBLE|HOST_COHERENT type. Real-backend
// canaries share this policy so each new graphics fixture cannot grow a subtly different copy.
inline long long pick_type(const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse& props,
                           std::uint64_t type_bits, bool device_local) {
    for (std::size_t i = 0; i < props.types.size() && i < 32; ++i) {
        if ((type_bits & (std::uint64_t{1} << i)) == 0) {
            continue;
        }
        const std::uint64_t flags = props.types[i].property_flags;
        const bool coherent = (flags & vkrpc::kMemoryPropertyHostVisible) != 0 &&
                              (flags & vkrpc::kMemoryPropertyHostCoherent) != 0;
        if ((device_local && (flags & vkrpc::kMemoryPropertyDeviceLocal) != 0) ||
            (!device_local && coherent)) {
            return static_cast<long long>(i);
        }
    }
    return -1;
}

// Shared real-backend discovery/device spine. Tests retain ownership of resource creation and
// teardown, while the repeated instance -> physical device -> logical device -> queue -> memory
// properties sequence has one implementation. Callers may inspect physical-device capabilities
// between discover() and create_device() and supply their exact feature request.
struct RealDeviceFixture {
    worker::RealVulkanBackend& backend;
    vkrpc::CreateInstanceResponse instance;
    vkrpc::EnumeratePhysicalDevicesResponse devices;
    std::uint64_t physical_device = 0;
    vkrpc::CreateDeviceResponse device;
    vkrpc::GetDeviceQueueResponse queue;
    vkrpc::GetPhysicalDeviceMemoryPropertiesResponse memory_properties;

    explicit RealDeviceFixture(worker::RealVulkanBackend& backend_in) : backend(backend_in) {}

    bool discover(std::string& reason) {
        instance = backend.create_instance({});
        if (!instance.ok) {
            reason = "no instance: " + instance.reason;
            return false;
        }
        vkrpc::EnumeratePhysicalDevicesRequest enumerate;
        enumerate.instance = instance.instance;
        devices = backend.enumerate_physical_devices(enumerate);
        if (!devices.ok || devices.devices.empty()) {
            reason = "no physical device";
            return false;
        }
        physical_device = devices.devices.front().handle;
        return true;
    }

    bool create_device(vkrpc::CreateDeviceRequest request, std::string& reason) {
        request.instance = instance.instance;
        request.physical_device = physical_device;
        device = backend.create_device(request);
        if (!device.ok) {
            reason = "create device: " + device.reason;
            return false;
        }
        vkrpc::GetDeviceQueueRequest get_queue;
        get_queue.device = device.device;
        get_queue.queue_family_index = device.queue_family_index;
        queue = backend.get_device_queue(get_queue);
        if (!queue.ok) {
            reason = "get device queue: " + queue.reason;
            return false;
        }
        vkrpc::GetPhysicalDeviceMemoryPropertiesRequest get_memory;
        get_memory.physical_device = physical_device;
        memory_properties = backend.get_physical_device_memory_properties(get_memory);
        if (!memory_properties.ok) {
            reason = "get memory properties: " + memory_properties.reason;
            return false;
        }
        return true;
    }
};

} // namespace vkr::test

#endif // VKRELAY2_TESTS_REAL_BACKEND_TEST_UTILS_HPP
