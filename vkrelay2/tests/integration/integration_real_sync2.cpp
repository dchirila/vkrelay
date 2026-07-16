// VK_KHR_synchronization2: in-process real-backend test on the host GPU.
//
// Constructs RealVulkanBackend directly and drives the sync2 command replay + the sync2 submit path
// against the real host driver, no daemon: create a device with VK_KHR_synchronization2 enabled
// (the six *KHR PFNs resolve, fail-closed if not), record a vkCmdPipelineBarrier2 (a global
// VkMemoryBarrier2 with 64-bit masks) + a vkCmdSetEvent2 on a VkEvent, submit the whole thing via
// vkQueueSubmit2 (one VkSubmitInfo2 + fence), wait, and assert the event reads VK_EVENT_SET --
// proof the sync2 barrier2 + event2 commands and the sync2 submit path all reached the host. The
// compute-read-after-write ORDERING proof + the timestamp2 path live in the sync2 canary; this pins
// the backend replay in the ctest gate without a compute pipeline.
//
// Windows-only (WIN32 + Vulkan SDK); skips gracefully with no usable device or no host sync2.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <cstdio>

#include <vulkan/vulkan.h>

using namespace vkr;

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_sync2: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_sync2");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_sync2: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_sync2");
    }
    // Enable VK_KHR_synchronization2 + its feature (the ICD would send both on the native lane).
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    cdr.enabled_extensions = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
    cdr.synchronization2_feature_enabled = 1;
    // The worker rebuilds the enabled-feature chain from enabled_feature_chain; feed the sync2
    // feature struct so the real device actually enables synchronization2.
    VkPhysicalDeviceSynchronization2Features s2{};
    s2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    s2.synchronization2 = VK_TRUE;
    vkrpc::CapabilityChainEntry e;
    e.s_type =
        static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
    e.size = sizeof(s2);
    e.blob.assign(reinterpret_cast<const char*>(&s2), sizeof(s2));
    cdr.enabled_feature_chain.push_back(e);
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    if (!cd.ok) {
        // A host without VK_KHR_synchronization2 (or where the *KHR PFNs do not resolve) is a clean
        // skip, not a failure -- the fail-closed create_device guard is doing its job.
        std::fprintf(stderr, "integration_real_sync2: skipped (no host sync2: %s)\n",
                     cd.reason.c_str());
        return vkr::test::finish("integration_real_sync2");
    }
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    vkrpc::CreateEventRequest cev;
    cev.device = cd.device;
    const vkrpc::CreateEventResponse event = backend.create_event(cev);
    VKR_CHECK(event.ok);
    vkrpc::CreateFenceRequest cf;
    cf.device = cd.device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(cf);
    VKR_CHECK(fence.ok);

    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    VKR_CHECK(pool.ok);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs.ok && bufs.command_buffers.size() == 1);

    // Record: a global VkMemoryBarrier2 (64-bit masks) via pipeline_barrier2, then set_event2.
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = bufs.command_buffers[0];
    rec.one_time_submit = true;
    vkrpc::RecordedCommand b2;
    b2.kind = "pipeline_barrier2";
    vkrpc::DependencyInfo2 dep;
    // ALL_COMMANDS -> ALL_COMMANDS, MEMORY_WRITE -> MEMORY_READ (all legal, single global barrier).
    dep.memory.push_back({static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
                          static_cast<std::uint64_t>(VK_ACCESS_2_MEMORY_WRITE_BIT),
                          static_cast<std::uint64_t>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
                          static_cast<std::uint64_t>(VK_ACCESS_2_MEMORY_READ_BIT)});
    b2.deps2.push_back(dep);
    rec.commands.push_back(b2);
    vkrpc::RecordedCommand se2;
    se2.kind = "set_event2";
    se2.args_u64 = {event.event};
    se2.deps2.push_back(vkrpc::DependencyInfo2{}); // empty dependency
    rec.commands.push_back(se2);
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    // Submit the whole thing via vkQueueSubmit2 (one VkSubmitInfo2 + the fence).
    vkrpc::QueueSubmit2Request s2r;
    s2r.queue = q.queue;
    vkrpc::SubmitInfo2 si;
    si.command_buffers = {bufs.command_buffers[0]};
    s2r.submits.push_back(si);
    s2r.fence = fence.fence;
    const vkrpc::QueueSubmitResponse sub = backend.queue_submit2(s2r);
    VKR_CHECK(sub.ok);
    VKR_CHECK_EQ(sub.result, static_cast<int>(VK_SUCCESS));

    vkrpc::WaitForFencesRequest wf;
    wf.fences = {fence.fence};
    wf.wait_all = true;
    wf.timeout = UINT64_MAX;
    const vkrpc::WaitForFencesResponse wr = backend.wait_for_fences(wf);
    VKR_CHECK(wr.ok && wr.result == static_cast<int>(VK_SUCCESS));

    // The sync2 set_event2 reached the host: the event is SET.
    vkrpc::HandleRequest h_event;
    h_event.handle = event.event;
    const vkrpc::GetEventStatusResponse ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, static_cast<int>(VK_EVENT_SET));

    std::fprintf(stderr, "integration_real_sync2: vkCmdPipelineBarrier2 + vkCmdSetEvent2 + "
                         "vkQueueSubmit2 all served on the real GPU\n");
    return vkr::test::finish("integration_real_sync2");
}
