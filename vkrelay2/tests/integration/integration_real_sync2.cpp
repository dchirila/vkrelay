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

    // Generation-scoped buffer lease: logical destroy happens before the record request reaches
    // the worker, yet only the pre-announced generation can resolve the retired host buffer.
    vkrpc::CreateBufferRequest create_buffer;
    create_buffer.device = cd.device;
    create_buffer.size = 64;
    create_buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    create_buffer.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
    const auto buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(buffer.ok);
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest memory_query;
    memory_query.physical_device = en.devices.front().handle;
    const auto memory_properties = backend.get_physical_device_memory_properties(memory_query);
    VKR_CHECK(memory_properties.ok);
    int memory_type = -1;
    for (std::size_t i = 0; i < memory_properties.types.size() && i < 32; ++i) {
        const std::uint64_t flags = memory_properties.types[i].property_flags;
        const bool admitted = (flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) == 0 &&
                              ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
                               (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);
        if (admitted && (buffer.mem_type_bits & (1ull << i)) != 0) {
            memory_type = static_cast<int>(i);
            break;
        }
    }
    VKR_CHECK(memory_type >= 0);
    vkrpc::AllocateMemoryRequest allocate_memory;
    allocate_memory.device = cd.device;
    allocate_memory.allocation_size = buffer.mem_size;
    allocate_memory.memory_type_index = memory_type;
    const auto memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(memory.ok);
    VKR_CHECK(backend.bind_buffer_memory({buffer.buffer, memory.memory, 0}).ok);

    vkrpc::LeasedDestroyRequest leased_destroy;
    leased_destroy.handle = buffer.buffer;
    // Generation 1 was begun and abandoned without an End RPC. The worker still knows epoch 0, so
    // both the pre-record destroy and the first delivered record at epoch 2 prove that generations
    // are monotonic identities rather than a contiguous worker-visible sequence.
    leased_destroy.leases = {{bufs.command_buffers[0], 2}};
    VKR_CHECK(backend.destroy_buffer_leased(leased_destroy).ok);
    vkrpc::RecordCommandBufferRequest leased_record;
    leased_record.command_buffer = bufs.command_buffers[0];
    leased_record.recording_generation = 2;
    vkrpc::RecordedCommand leased_barrier;
    leased_barrier.kind = "pipeline_barrier2";
    vkrpc::DependencyInfo2 leased_dependency;
    vkrpc::BufferMemoryBarrier2 buffer_barrier;
    buffer_barrier.src_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    buffer_barrier.dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    buffer_barrier.src_queue_family = vkrpc::kVkQueueFamilyIgnored;
    buffer_barrier.dst_queue_family = vkrpc::kVkQueueFamilyIgnored;
    buffer_barrier.buffer = buffer.buffer;
    buffer_barrier.offset = 0;
    buffer_barrier.size = 64;
    leased_dependency.buffer.push_back(buffer_barrier);
    leased_barrier.deps2.push_back(leased_dependency);
    leased_record.commands.push_back(leased_barrier);
    VKR_CHECK(backend.record_command_buffer(leased_record).ok);
    vkrpc::QueueSubmit2Request leased_submit;
    leased_submit.queue = q.queue;
    vkrpc::SubmitInfo2 leased_info;
    leased_info.command_buffers = {bufs.command_buffers[0]};
    leased_submit.submits.push_back(leased_info);
    VKR_CHECK(backend.queue_submit2(leased_submit).ok);
    VKR_CHECK(backend.device_wait_idle({cd.device}).ok);
    vkrpc::RetireCommandBufferRecordingsRequest retire;
    retire.recordings = {{bufs.command_buffers[0], 2}};
    VKR_CHECK(backend.retire_command_buffer_recordings(retire).ok);
    VKR_CHECK(backend.free_memory({memory.memory}).ok);

    vkrpc::RecordCommandBufferRequest stale_record = leased_record;
    stale_record.recording_generation = 1;
    const vkrpc::StatusResponse stale_response = backend.record_command_buffer(stale_record);
    VKR_CHECK(!stale_response.ok);
    VKR_CHECK_EQ(stale_response.reason, "recording generation is stale");

    // The after-successful-record twin: a leased destroy of the current generation must preserve
    // its host command buffer and keep it submittable.
    const auto post_record_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(post_record_buffer.ok);
    allocate_memory.allocation_size = post_record_buffer.mem_size;
    const auto post_record_memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(post_record_memory.ok);
    VKR_CHECK(
        backend.bind_buffer_memory({post_record_buffer.buffer, post_record_memory.memory, 0}).ok);
    // Epoch 3 is also abandoned locally; the next delivered epoch 4 remains valid.
    leased_record.recording_generation = 4;
    leased_record.commands[0].deps2[0].buffer[0].buffer = post_record_buffer.buffer;
    VKR_CHECK(backend.record_command_buffer(leased_record).ok);
    leased_destroy.handle = post_record_buffer.buffer;
    leased_destroy.leases = {{bufs.command_buffers[0], 4}};
    VKR_CHECK(backend.destroy_buffer_leased(leased_destroy).ok);
    VKR_CHECK(backend.queue_submit2(leased_submit).ok);
    VKR_CHECK(backend.device_wait_idle({cd.device}).ok);
    retire.recordings = {{bufs.command_buffers[0], 4}};
    VKR_CHECK(backend.retire_command_buffer_recordings(retire).ok);
    VKR_CHECK(backend.free_memory({post_record_memory.memory}).ok);

    // Session-abort/device-teardown backstop: deliberately leave one logically retired resource
    // and allocation behind. RealVulkanBackend's destructor must wait idle, destroy the retired
    // host buffer (which is absent from the live map), then free its memory before the device.
    const auto teardown_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(teardown_buffer.ok);
    allocate_memory.allocation_size = teardown_buffer.mem_size;
    const auto teardown_memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(teardown_memory.ok);
    VKR_CHECK(backend.bind_buffer_memory({teardown_buffer.buffer, teardown_memory.memory, 0}).ok);
    leased_destroy.handle = teardown_buffer.buffer;
    leased_destroy.leases = {{bufs.command_buffers[0], 5}};
    VKR_CHECK(backend.destroy_buffer_leased(leased_destroy).ok);
    VKR_CHECK(backend.free_memory({teardown_memory.memory}).ok);
    const vkrpc::LifetimeLeaseStats teardown_stats = backend.lifetime_lease_stats();
    VKR_CHECK_EQ(teardown_stats.retired_resources, static_cast<std::size_t>(1));
    VKR_CHECK_EQ(teardown_stats.retired_memories, static_cast<std::size_t>(1));
    VKR_CHECK_EQ(teardown_stats.lease_edges, static_cast<std::size_t>(1));

    std::fprintf(stderr, "integration_real_sync2: sync2 replay/submit and generation-scoped "
                         "buffer leases all served on the real GPU\n");
    return vkr::test::finish("integration_real_sync2");
}
