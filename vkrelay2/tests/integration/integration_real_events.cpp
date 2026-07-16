// Core-1.0 synchronization objects: in-process real-backend test on the host GPU.
//
// Constructs RealVulkanBackend directly and exercises the honest-VkResult host-op paths the relay
// carries, no complex GPU workload:
//   - vkGetFenceStatus: a signaled-create fence forwards VK_SUCCESS, an unsignaled one
//   VK_NOT_READY;
//     a submit that carries the fence signals it (VK_SUCCESS after the wait); a reset returns it to
//     VK_NOT_READY -- the driver's real VkResult forwarded verbatim (ok=true), not a fault.
//   - the VkEvent object model: create -> VK_EVENT_RESET, vkSetEvent -> VK_EVENT_SET, vkResetEvent
//     -> VK_EVENT_RESET, plus wrong-kind rejects (a fence handle is not an event).
//   - a submitted vkCmdSetEvent: recorded + replayed on the real GPU, the host status reads
//     VK_EVENT_SET (the device set reached the host through record_command_buffer + queue_submit).
//
// Windows-only (WIN32 + Vulkan SDK); skips gracefully with no usable device. Validation-clean gate:
// a manual run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
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
        std::fprintf(stderr, "integration_real_events: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_events");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_events: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_events");
    }
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    if (!cd.ok) {
        std::fprintf(stderr, "integration_real_events: skipped (no device: %s)\n",
                     cd.reason.c_str());
        return vkr::test::finish("integration_real_events");
    }
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    // --- vkGetFenceStatus forwards the driver's real VkResult. ---
    vkrpc::CreateFenceRequest cf_sig;
    cf_sig.device = cd.device;
    cf_sig.signaled = true;
    const vkrpc::CreateFenceResponse fence_sig = backend.create_fence(cf_sig);
    VKR_CHECK(fence_sig.ok);
    vkrpc::HandleRequest h_fsig;
    h_fsig.handle = fence_sig.fence;
    vkrpc::GetFenceStatusResponse gfs = backend.get_fence_status(h_fsig);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, static_cast<int>(VK_SUCCESS));

    vkrpc::CreateFenceRequest cf;
    cf.device = cd.device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(cf);
    VKR_CHECK(fence.ok);
    vkrpc::HandleRequest h_fence;
    h_fence.handle = fence.fence;
    gfs = backend.get_fence_status(h_fence);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, static_cast<int>(VK_NOT_READY));

    // --- The VkEvent object model on the real driver. ---
    vkrpc::CreateEventRequest ce;
    ce.device = cd.device;
    const vkrpc::CreateEventResponse event = backend.create_event(ce);
    VKR_CHECK(event.ok && event.event != 0);
    vkrpc::HandleRequest h_event;
    h_event.handle = event.event;
    vkrpc::GetEventStatusResponse ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, static_cast<int>(VK_EVENT_RESET));
    VKR_CHECK(backend.set_event(h_event).ok);
    ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, static_cast<int>(VK_EVENT_SET));
    VKR_CHECK(backend.reset_event(h_event).ok);
    ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, static_cast<int>(VK_EVENT_RESET));

    // Wrong-kind rejects: a fence handle is not an event, and an event is not a fence.
    VKR_CHECK(!backend.get_event_status(h_fence).ok);
    VKR_CHECK(!backend.set_event(h_fence).ok);
    VKR_CHECK(!backend.get_fence_status(h_event).ok);

    // --- A submitted vkCmdSetEvent reaches the host: record set_event, submit with the fence,
    // wait, and the host status reads VK_EVENT_SET; the fence also signals (VK_SUCCESS). ---
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

    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = bufs.command_buffers[0];
    rec.one_time_submit = true;
    vkrpc::RecordedCommand set_ev;
    set_ev.kind = "set_event";
    set_ev.src_stage = static_cast<long long>(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    set_ev.args_u64 = {event.event};
    rec.commands.push_back(set_ev);
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    sub.command_buffers = {bufs.command_buffers[0]};
    sub.fence = fence.fence;
    const vkrpc::QueueSubmitResponse s = backend.queue_submit(sub);
    VKR_CHECK(s.ok);
    vkrpc::WaitForFencesRequest wf;
    wf.fences = {fence.fence};
    wf.wait_all = true;
    wf.timeout = UINT64_MAX;
    const vkrpc::WaitForFencesResponse wr = backend.wait_for_fences(wf);
    VKR_CHECK(wr.ok);
    VKR_CHECK_EQ(wr.result, static_cast<int>(VK_SUCCESS));

    gfs = backend.get_fence_status(h_fence);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, static_cast<int>(VK_SUCCESS));
    ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, static_cast<int>(VK_EVENT_SET));

    // Reset the fence and confirm vkGetFenceStatus follows back to unsignaled.
    vkrpc::ResetFencesRequest rf;
    rf.fences = {fence.fence};
    VKR_CHECK(backend.reset_fences(rf).ok);
    gfs = backend.get_fence_status(h_fence);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, static_cast<int>(VK_NOT_READY));

    // Teardown: destroy the event (double-destroy rejected).
    VKR_CHECK(backend.destroy_event(h_event).ok);
    VKR_CHECK(!backend.destroy_event(h_event).ok);

    std::fprintf(stderr,
                 "integration_real_events: event object + host set/reset/status + "
                 "vkGetFenceStatus + a submitted vkCmdSetEvent all served on the real GPU\n");
    return vkr::test::finish("integration_real_events");
}
