// Real (loader-backed) Vulkan capability negotiation end to end.
// Launches a worker forced to the real backend (--vulkan-backend real), drives
// the full app -> supervisor -> worker -> real Vulkan -> app path, and asserts
// the worker reports a real device's capabilities. Windows-only (registered by
// CMake under WIN32 + Vulkan SDK). Skips gracefully (still passes) if no usable
// Vulkan device is present, so it is safe on a headless/driverless box.
#include "windows/supervisor/worker_supervisor.hpp"

#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/vkrpc/rpc.hpp"
#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace vkr;

namespace {

constexpr char kAppId[] = "integration-real-app";

protocol::SessionStarted launch_session(transport::MessageChannel& channel,
                                        const std::string& gpu_selector) {
    protocol::LaunchSession req;
    req.app_instance_id = kAppId;
    req.gpu_selector = gpu_selector;
    channel.send(protocol::MessageType::LaunchSession, req.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body) || type != protocol::MessageType::SessionStarted) {
        throw std::runtime_error("expected session_started");
    }
    return protocol::SessionStarted::from_body(body);
}

std::unique_ptr<transport::Connection> open_data_plane(const std::string& host, int port,
                                                       const std::string& token) {
    auto conn = transport::tcp_connect(host, port);
    transport::MessageChannel channel(*conn);
    protocol::AppHello hello;
    hello.app_token = token;
    channel.send(protocol::MessageType::AppHello, hello.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body) || type != protocol::MessageType::AppAck) {
        throw std::runtime_error("no app_ack");
    }
    if (!protocol::AppAck::from_body(body).ok) {
        throw std::runtime_error("app_ack not ok");
    }
    return conn;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: integration_vulkan_real <worker-path>\n");
        return 2;
    }

    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = argv[1];
    cfg.heartbeat_timeout_ms = 5000;
    cfg.worker_interval_ms = 50;
    cfg.worker_count = 0;
    cfg.extra_worker_args = {"--vulkan-backend", "real"}; // force the real backend

    supervisor::WorkerSupervisor sup(cfg);

    auto app_listener = transport::tcp_listen(0);
    const int app_port = app_listener->port();
    protocol::IdAllocator ids;
    const auto devices = protocol::probe_mocked();

    std::thread server([&] {
        try {
            auto conn = app_listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, ids, devices, &sup);
        } catch (const std::exception&) {
        }
    });

    try {
        auto conn = transport::tcp_connect("127.0.0.1", app_port);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = kAppId;
        transport::client_handshake(channel, hello);

        const protocol::SessionStarted s = launch_session(channel, "auto");
        VKR_CHECK(!s.worker_id.empty());
        VKR_CHECK(s.data_plane_port > 0);

        auto dp = open_data_plane(s.data_plane_host, s.data_plane_port, s.app_token);
        vkrpc::RpcChannel rpc(*dp);

        vkrpc::CapabilitiesRequest req;
        req.requested_api_major = 1;
        req.requested_api_minor = 3;
        const vkrpc::CapabilitiesResponse caps = vkrpc::negotiate_capabilities(rpc, 1, req);

        if (!caps.ok) {
            // No usable Vulkan on this box -- a legitimate skip, not a failure.
            std::fprintf(stderr, "integration_vulkan_real: skipped (no usable Vulkan: %s)\n",
                         caps.reason.c_str());
        } else {
            // The real loader reported a real device over the full RPC path.
            VKR_CHECK(!caps.device.device_name.empty());
            VKR_CHECK(caps.negotiated_api_major >= 1);
            VKR_CHECK(caps.negotiated_api_minor >= 0);
            VKR_CHECK(caps.negotiated_api_minor <= vkrpc::kSupportedApiMinor ||
                      caps.negotiated_api_major < vkrpc::kSupportedApiMajor);
            std::fprintf(stderr, "integration_vulkan_real: real device '%s' negotiated %d.%d\n",
                         caps.device.device_name.c_str(), caps.negotiated_api_major,
                         caps.negotiated_api_minor);

            // Real instance/device spine: create_instance -> enumerate ->
            // create_device on the real driver, with selection + destroy ordering.
            const vkrpc::CreateInstanceResponse ci = vkrpc::create_instance(rpc, 2, {});
            VKR_CHECK(ci.ok);
            VKR_CHECK(ci.instance != 0);

            vkrpc::EnumeratePhysicalDevicesRequest er;
            er.instance = ci.instance;
            const vkrpc::EnumeratePhysicalDevicesResponse en =
                vkrpc::enumerate_physical_devices(rpc, 3, er);
            VKR_CHECK(en.ok);
            VKR_CHECK_EQ(en.devices.size(), static_cast<std::size_t>(1));
            VKR_CHECK(!en.devices.front().caps.device_name.empty());
            const auto phys = en.devices.front().handle;

            vkrpc::CreateDeviceRequest cdr;
            cdr.instance = ci.instance;
            cdr.physical_device = phys;
            const vkrpc::CreateDeviceResponse cd = vkrpc::create_device(rpc, 4, cdr);
            VKR_CHECK(cd.ok);
            VKR_CHECK(cd.device != 0);
            VKR_CHECK(cd.queue_count >= 1);

            // Selection enforced: a physical device not enumerated from this
            // instance is rejected.
            vkrpc::CreateDeviceRequest cdr_bad;
            cdr_bad.instance = ci.instance;
            cdr_bad.physical_device = phys + 1;
            VKR_CHECK(!vkrpc::create_device(rpc, 5, cdr_bad).ok);

            // Real queue: the (family, index) create_device reported retrieves a
            // queue on the real driver, with a stable handle across calls.
            vkrpc::GetDeviceQueueRequest gq;
            gq.device = cd.device;
            gq.queue_family_index = cd.queue_family_index;
            gq.queue_index = 0;
            const vkrpc::GetDeviceQueueResponse q = vkrpc::get_device_queue(rpc, 6, gq);
            VKR_CHECK(q.ok);
            VKR_CHECK(q.queue != 0);
            VKR_CHECK_EQ(vkrpc::get_device_queue(rpc, 7, gq).queue, q.queue);
            std::fprintf(stderr,
                         "integration_vulkan_real: real queue on '%s' "
                         "family=%d count=%d -> handle=%llu\n",
                         caps.device.device_name.c_str(), cd.queue_family_index, cd.queue_count,
                         static_cast<unsigned long long>(q.queue));

            // A family/index the device never created is rejected (would be UB on
            // the real driver): wrong family, and an out-of-range index.
            vkrpc::GetDeviceQueueRequest gq_badfamily = gq;
            gq_badfamily.queue_family_index = cd.queue_family_index + 1000;
            VKR_CHECK(!vkrpc::get_device_queue(rpc, 8, gq_badfamily).ok);
            vkrpc::GetDeviceQueueRequest gq_badindex = gq;
            gq_badindex.queue_index = cd.queue_count;
            VKR_CHECK(!vkrpc::get_device_queue(rpc, 9, gq_badindex).ok);

            // Real command pool + buffers on the host driver, using the family
            // create_device reported.
            vkrpc::CreateCommandPoolRequest cpr;
            cpr.device = cd.device;
            cpr.queue_family_index = cd.queue_family_index;
            const vkrpc::CreateCommandPoolResponse pool = vkrpc::create_command_pool(rpc, 10, cpr);
            VKR_CHECK(pool.ok);
            VKR_CHECK(pool.command_pool != 0);

            // A pool for a family the device didn't create queues for is rejected.
            vkrpc::CreateCommandPoolRequest cpr_bad = cpr;
            cpr_bad.queue_family_index = cd.queue_family_index + 1000;
            VKR_CHECK(!vkrpc::create_command_pool(rpc, 11, cpr_bad).ok);

            vkrpc::AllocateCommandBuffersRequest abr;
            abr.command_pool = pool.command_pool;
            abr.count = 2;
            const vkrpc::AllocateCommandBuffersResponse bufs =
                vkrpc::allocate_command_buffers(rpc, 12, abr);
            VKR_CHECK(bufs.ok);
            VKR_CHECK_EQ(bufs.command_buffers.size(), static_cast<std::size_t>(2));

            // Free one; a batch repeating a live handle is rejected before any free.
            vkrpc::FreeCommandBuffersRequest fbr;
            fbr.command_pool = pool.command_pool;
            fbr.command_buffers = {bufs.command_buffers[0]};
            VKR_CHECK(vkrpc::free_command_buffers(rpc, 13, fbr).ok);
            vkrpc::FreeCommandBuffersRequest fbr_dup;
            fbr_dup.command_pool = pool.command_pool;
            fbr_dup.command_buffers = {bufs.command_buffers[1], bufs.command_buffers[1]};
            VKR_CHECK(!vkrpc::free_command_buffers(rpc, 14, fbr_dup).ok);

            // A device with a live command pool can't be destroyed (child-first).
            vkrpc::HandleRequest dd_early;
            dd_early.handle = cd.device;
            VKR_CHECK(!vkrpc::destroy_device(rpc, 15, dd_early).ok);

            // Destroy the pool (frees its remaining buffer).
            vkrpc::HandleRequest dpool;
            dpool.handle = pool.command_pool;
            VKR_CHECK(vkrpc::destroy_command_pool(rpc, 16, dpool).ok);

            // Real device-child sync + memory objects on the host driver.
            vkrpc::CreateFenceRequest cfr;
            cfr.device = cd.device;
            const vkrpc::CreateFenceResponse fence = vkrpc::create_fence(rpc, 17, cfr);
            VKR_CHECK(fence.ok);
            VKR_CHECK(fence.fence != 0);
            vkrpc::CreateSemaphoreRequest csr;
            csr.device = cd.device;
            const vkrpc::CreateSemaphoreResponse sem = vkrpc::create_semaphore(rpc, 18, csr);
            VKR_CHECK(sem.ok);
            VKR_CHECK(sem.semaphore != 0);
            // Pick a real memory type the worker admits (memory-class gate: a type must
            // be HOST_VISIBLE|HOST_COHERENT or DEVICE_LOCAL-only -- NVIDIA exposes a propertyFlags
            // == 0 type at index 0, which is no longer blindly allocatable).
            vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
            mpr.physical_device = phys;
            const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mprops =
                vkrpc::get_physical_device_memory_properties(rpc, 19, mpr);
            VKR_CHECK(mprops.ok && !mprops.types.empty());
            long long mem_type = -1;
            for (std::size_t i = 0; i < mprops.types.size(); ++i) {
                const std::uint64_t f = mprops.types[i].property_flags;
                const bool hv = (f & vkrpc::kMemoryPropertyHostVisible) != 0;
                const bool hc = (f & vkrpc::kMemoryPropertyHostCoherent) != 0;
                const bool dl = (f & vkrpc::kMemoryPropertyDeviceLocal) != 0;
                if (hv ? hc : dl) {
                    mem_type = static_cast<long long>(i);
                    break;
                }
            }
            VKR_CHECK(mem_type >= 0);
            vkrpc::AllocateMemoryRequest amr;
            amr.device = cd.device;
            amr.allocation_size = 256;
            amr.memory_type_index = mem_type;
            const vkrpc::AllocateMemoryResponse mem = vkrpc::allocate_memory(rpc, 19, amr);
            VKR_CHECK(mem.ok);
            VKR_CHECK(mem.memory != 0);

            // An out-of-range memory type index is rejected (real memoryTypeCount).
            vkrpc::AllocateMemoryRequest amr_bad = amr;
            amr_bad.memory_type_index = 100000;
            VKR_CHECK(!vkrpc::allocate_memory(rpc, 20, amr_bad).ok);

            // A typed destroy rejects a wrong-kind handle (no cross-freeing).
            vkrpc::HandleRequest free_fence_as_mem;
            free_fence_as_mem.handle = fence.fence;
            VKR_CHECK(!vkrpc::free_memory(rpc, 21, free_fence_as_mem).ok);
            vkrpc::HandleRequest destroy_mem_as_fence;
            destroy_mem_as_fence.handle = mem.memory;
            VKR_CHECK(!vkrpc::destroy_fence(rpc, 22, destroy_mem_as_fence).ok);

            // A device with live leaf objects can't be destroyed (child-first).
            vkrpc::HandleRequest dd_leaf;
            dd_leaf.handle = cd.device;
            VKR_CHECK(!vkrpc::destroy_device(rpc, 23, dd_leaf).ok);

            // Free the leaves, then teardown ordering.
            vkrpc::HandleRequest df;
            df.handle = fence.fence;
            VKR_CHECK(vkrpc::destroy_fence(rpc, 24, df).ok);
            vkrpc::HandleRequest ds;
            ds.handle = sem.semaphore;
            VKR_CHECK(vkrpc::destroy_semaphore(rpc, 25, ds).ok);
            vkrpc::HandleRequest dm;
            dm.handle = mem.memory;
            VKR_CHECK(vkrpc::free_memory(rpc, 26, dm).ok);

            // Real WSI plumbing: a surface (hidden Win32 window +
            // VkSurfaceKHR), a real swapchain on it, and its images -- no present yet.
            vkrpc::CreateSurfaceRequest sr;
            sr.instance = ci.instance;
            const vkrpc::CreateSurfaceResponse surf = vkrpc::create_surface(rpc, 30, sr);
            VKR_CHECK(surf.ok);
            VKR_CHECK(surf.surface != 0);

            // WSI capability queries over the real RPC path (the exact flow the ICD uses):
            // query caps/formats/present-modes/support, then choose the swapchain params from
            // them rather than hardcoding -- proving the path end to end on the host driver.
            vkrpc::GetSurfaceCapabilitiesRequest scap_req;
            scap_req.physical_device = phys;
            scap_req.surface = surf.surface;
            const vkrpc::GetSurfaceCapabilitiesResponse scap =
                vkrpc::get_surface_capabilities(rpc, 70, scap_req);
            VKR_CHECK(scap.ok);
            VKR_CHECK_EQ(scap.current_extent_width,
                         vkrpc::kDynamicExtentSentinel); // dynamic-extent sentinel
            vkrpc::GetSurfaceFormatsRequest sfmt_req;
            sfmt_req.physical_device = phys;
            sfmt_req.surface = surf.surface;
            const vkrpc::GetSurfaceFormatsResponse sfmt =
                vkrpc::get_surface_formats(rpc, 71, sfmt_req);
            VKR_CHECK(sfmt.ok && !sfmt.formats.empty());
            vkrpc::GetSurfacePresentModesRequest spm_req;
            spm_req.physical_device = phys;
            spm_req.surface = surf.surface;
            const vkrpc::GetSurfacePresentModesResponse spm =
                vkrpc::get_surface_present_modes(rpc, 72, spm_req);
            VKR_CHECK(spm.ok && !spm.present_modes.empty());
            vkrpc::GetSurfaceSupportRequest ssup_req;
            ssup_req.physical_device = phys;
            ssup_req.queue_family_index = 0;
            ssup_req.surface = surf.surface;
            const vkrpc::GetSurfaceSupportResponse ssup =
                vkrpc::get_surface_support(rpc, 73, ssup_req);
            VKR_CHECK(ssup.ok && ssup.supported);

            // Pick format (prefer B8G8R8A8_UNORM/SRGB), present mode (prefer FIFO), and usage
            // from the queried caps. TRANSFER_DST (the clear needs it) is taken only if the
            // surface advertises it -- honest, caps-driven, no create-time guessing.
            int chosen_format = sfmt.formats.front().format;
            int chosen_color_space = sfmt.formats.front().color_space;
            for (const auto& f : sfmt.formats) {
                if (f.format == 44 /*B8G8R8A8_UNORM*/ && f.color_space == 0 /*SRGB_NONLINEAR*/) {
                    chosen_format = f.format;
                    chosen_color_space = f.color_space;
                    break;
                }
            }
            int chosen_present_mode = spm.present_modes.front();
            for (const int m : spm.present_modes) {
                if (m == 2 /*FIFO*/) {
                    chosen_present_mode = m;
                    break;
                }
            }
            const bool can_clear = (scap.supported_usage_flags &
                                    static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst)) != 0;

            vkrpc::CreateSwapchainRequest scr;
            scr.device = cd.device;
            scr.surface = surf.surface;
            scr.image_format = chosen_format;
            scr.color_space = chosen_color_space;
            scr.present_mode = chosen_present_mode;
            scr.width = 256;
            scr.height = 256;
            // Request at least 2 images (double-buffer), but clamp to the surface maximum so
            // the request stays satisfiable on any caps (the assertion below checks against
            // this requested minimum, not a hardcoded 2).
            std::uint64_t want_images = scap.min_image_count < 2 ? 2 : scap.min_image_count;
            if (scap.max_image_count != 0 && want_images > scap.max_image_count) {
                want_images = scap.max_image_count;
            }
            scr.min_image_count = static_cast<int>(want_images);
            scr.image_usage =
                can_clear ? (vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst)
                          : vkrpc::kImageUsageColorAttachment;
            const vkrpc::CreateSwapchainResponse sc = vkrpc::create_swapchain(rpc, 31, scr);
            VKR_CHECK(sc.ok);
            VKR_CHECK(sc.swapchain != 0);

            // Validation: malformed/unsupported swapchain requests are rejected, never
            // silently substituted. A sentinel present-mode, an unsupported (UNDEFINED)
            // format, and a min_image_count above any real surface maximum all fail.
            vkrpc::CreateSwapchainRequest scr_sentinel = scr;
            scr_sentinel.present_mode = -1; // decoder sentinel for malformed
            VKR_CHECK(!vkrpc::create_swapchain(rpc, 42, scr_sentinel).ok);
            vkrpc::CreateSwapchainRequest scr_badfmt = scr;
            scr_badfmt.image_format = 0; // VK_FORMAT_UNDEFINED -> unsupported
            VKR_CHECK(!vkrpc::create_swapchain(rpc, 43, scr_badfmt).ok);
            vkrpc::CreateSwapchainRequest scr_toomany = scr;
            scr_toomany.min_image_count = 100000; // above any real surface maximum
            VKR_CHECK(!vkrpc::create_swapchain(rpc, 44, scr_toomany).ok);

            // get_swapchain_images: at least the requested minimum, all non-null,
            // stable across calls; an unknown swapchain is rejected. (Assert against the
            // requested min, not a hardcoded 2, so the test stays portable to a surface that
            // reports minImageCount == 1.)
            vkrpc::GetSwapchainImagesRequest gir;
            gir.swapchain = sc.swapchain;
            const vkrpc::GetSwapchainImagesResponse imgs =
                vkrpc::get_swapchain_images(rpc, 32, gir);
            VKR_CHECK(imgs.ok);
            VKR_CHECK(imgs.images.size() >= want_images);
            for (const auto img : imgs.images) {
                VKR_CHECK(img != 0);
            }
            const vkrpc::GetSwapchainImagesResponse imgs2 =
                vkrpc::get_swapchain_images(rpc, 33, gir);
            VKR_CHECK(imgs2.images == imgs.images);
            std::fprintf(stderr, "integration_vulkan_real: real swapchain with %zu images\n",
                         imgs.images.size());
            vkrpc::GetSwapchainImagesRequest gir_bad;
            gir_bad.swapchain = sc.swapchain + 999;
            VKR_CHECK(!vkrpc::get_swapchain_images(rpc, 34, gir_bad).ok);

            // App-recorded frame over the full RPC path: a command pool +
            // buffer, real binary semaphores + a fence, then acquire(signal) ->
            // record(barrier+clear+barrier) -> submit(wait/signal/fence) -> wait_for_fences
            // -> present(wait). The window shows on the first successful present. (One frame:
            // enough to prove the submit spine + real sync end to end; pipelined frames are
            // a later concern, and binary-semaphore reuse across frames needs more care.)
            vkrpc::CreateCommandPoolRequest fcpr;
            fcpr.device = cd.device;
            fcpr.queue_family_index = cd.queue_family_index;
            const vkrpc::CreateCommandPoolResponse fpool =
                vkrpc::create_command_pool(rpc, 50, fcpr);
            VKR_CHECK(fpool.ok);
            vkrpc::AllocateCommandBuffersRequest fabr;
            fabr.command_pool = fpool.command_pool;
            fabr.count = 1;
            const vkrpc::AllocateCommandBuffersResponse fbufs =
                vkrpc::allocate_command_buffers(rpc, 51, fabr);
            VKR_CHECK(fbufs.ok && fbufs.command_buffers.size() == 1);
            const std::uint64_t fcmd = fbufs.command_buffers[0];
            vkrpc::CreateSemaphoreRequest fcsr;
            fcsr.device = cd.device;
            const vkrpc::CreateSemaphoreResponse fsem_acq = vkrpc::create_semaphore(rpc, 52, fcsr);
            const vkrpc::CreateSemaphoreResponse fsem_done = vkrpc::create_semaphore(rpc, 53, fcsr);
            VKR_CHECK(fsem_acq.ok && fsem_done.ok);
            vkrpc::CreateFenceRequest fcfr;
            fcfr.device = cd.device;
            const vkrpc::CreateFenceResponse ffence = vkrpc::create_fence(rpc, 54, fcfr);
            VKR_CHECK(ffence.ok);

            // VK_* values sent over the wire as ints (named here; this test links no Vulkan).
            constexpr int kLayoutUndefined = 0;
            constexpr int kLayoutTransferDst = 7;         // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            constexpr int kLayoutPresentSrc = 1000001002; // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            constexpr long long kStageTopOfPipe = 0x00000001;
            constexpr long long kStageTransfer = 0x00001000;
            constexpr long long kStageBottomOfPipe = 0x00002000;
            constexpr long long kAccessTransferWrite = 0x00001000;
            constexpr int kAspectColor = 0x00000001;

            bool presented = false;
            if (can_clear) {
                vkrpc::AcquireNextImageRequest ar;
                ar.swapchain = sc.swapchain;
                ar.timeout = UINT64_MAX; // block for an image
                ar.semaphore = fsem_acq.semaphore;
                const vkrpc::AcquireNextImageResponse acq = vkrpc::acquire_next_image(rpc, 55, ar);
                VKR_CHECK(acq.ok);
                VKR_CHECK_EQ(acq.result, 0);
                VKR_CHECK(acq.image_index >= 0);
                const std::uint64_t image = imgs.images[acq.image_index];

                vkrpc::RecordCommandBufferRequest rec;
                rec.command_buffer = fcmd;
                rec.one_time_submit = true;
                vkrpc::RecordedCommand to_dst;
                to_dst.kind = "pipeline_barrier";
                to_dst.image = image;
                to_dst.old_layout = kLayoutUndefined;
                to_dst.new_layout = kLayoutTransferDst;
                to_dst.src_stage = kStageTopOfPipe;
                to_dst.dst_stage = kStageTransfer;
                to_dst.src_access = 0;
                to_dst.dst_access = kAccessTransferWrite;
                to_dst.aspect = kAspectColor;
                rec.commands.push_back(to_dst);
                vkrpc::RecordedCommand clear;
                clear.kind = "clear_color_image";
                clear.image = image;
                clear.layout = kLayoutTransferDst;
                clear.r = 0.12; // a recognizable blue
                clear.g = 0.56;
                clear.b = 1.0;
                clear.a = 1.0;
                rec.commands.push_back(clear);
                vkrpc::RecordedCommand to_present;
                to_present.kind = "pipeline_barrier";
                to_present.image = image;
                to_present.old_layout = kLayoutTransferDst;
                to_present.new_layout = kLayoutPresentSrc;
                to_present.src_stage = kStageTransfer;
                to_present.dst_stage = kStageBottomOfPipe;
                to_present.src_access = kAccessTransferWrite;
                to_present.dst_access = 0;
                to_present.aspect = kAspectColor;
                rec.commands.push_back(to_present);
                VKR_CHECK(vkrpc::record_command_buffer(rpc, 57, rec).ok);

                vkrpc::QueueSubmitRequest sub;
                sub.queue = q.queue;
                sub.waits.push_back({fsem_acq.semaphore, kStageTransfer});
                sub.command_buffers = {fcmd};
                sub.signal_semaphores = {fsem_done.semaphore};
                sub.fence = ffence.fence;
                const vkrpc::QueueSubmitResponse subr = vkrpc::queue_submit(rpc, 58, sub);
                VKR_CHECK(subr.ok);
                VKR_CHECK_EQ(subr.result, 0);

                vkrpc::WaitForFencesRequest wf;
                wf.fences = {ffence.fence};
                wf.wait_all = true;
                wf.timeout = UINT64_MAX;
                const vkrpc::WaitForFencesResponse wr = vkrpc::wait_for_fences(rpc, 59, wf);
                VKR_CHECK(wr.ok);
                VKR_CHECK_EQ(wr.result, 0);

                vkrpc::QueuePresentRequest pr;
                pr.queue = q.queue;
                pr.wait_semaphores = {fsem_done.semaphore};
                pr.presents.push_back({sc.swapchain, acq.image_index});
                const vkrpc::QueuePresentResponse pres = vkrpc::queue_present(rpc, 60, pr);
                VKR_CHECK(pres.ok);
                VKR_CHECK_EQ(static_cast<int>(pres.results.size()), 1);
                VKR_CHECK(pres.result == 0 || pres.result == 1000001003 /*SUBOPTIMAL*/);
                presented = true;
            } else {
                std::fprintf(stderr,
                             "integration_vulkan_real: clear unavailable (no TRANSFER_DST)\n");
            }
            std::fprintf(stderr, "integration_vulkan_real: presented %d app-recorded frame(s)\n",
                         presented ? 1 : 0);

            // Acquire/present RPC faults are ok=false (the op did not run).
            vkrpc::AcquireNextImageRequest ar_bad;
            ar_bad.swapchain = sc.swapchain + 999;
            VKR_CHECK(!vkrpc::acquire_next_image(rpc, 61, ar_bad).ok);
            vkrpc::QueuePresentRequest pr_badq;
            pr_badq.queue = q.queue + 999;
            pr_badq.presents.push_back({sc.swapchain, 0});
            VKR_CHECK(!vkrpc::queue_present(rpc, 62, pr_badq).ok);

            // A submit referencing a never-recorded command buffer is rejected (ok=false).
            const vkrpc::AllocateCommandBuffersResponse fbufs2 =
                vkrpc::allocate_command_buffers(rpc, 63, fabr);
            VKR_CHECK(fbufs2.ok);
            vkrpc::QueueSubmitRequest sub_unrec;
            sub_unrec.queue = q.queue;
            sub_unrec.command_buffers = {fbufs2.command_buffers[0]};
            VKR_CHECK(!vkrpc::queue_submit(rpc, 64, sub_unrec).ok);

            // Destroy ordering: a surface with a live swapchain, and a device with live
            // children, both refuse.
            vkrpc::HandleRequest h_surf;
            h_surf.handle = surf.surface;
            VKR_CHECK(!vkrpc::destroy_surface(rpc, 35, h_surf).ok);
            vkrpc::HandleRequest dd_swc;
            dd_swc.handle = cd.device;
            VKR_CHECK(!vkrpc::destroy_device(rpc, 36, dd_swc).ok);

            // Ordered teardown. destroy_swapchain idles the device (flushing the frame's
            // submit/present), so the sync leaves are safe to free; then pool + leaves,
            // surface, device, instance (child-before-parent).
            vkrpc::HandleRequest h_swc;
            h_swc.handle = sc.swapchain;
            VKR_CHECK(vkrpc::destroy_swapchain(rpc, 37, h_swc).ok);
            vkrpc::HandleRequest h_fpool;
            h_fpool.handle = fpool.command_pool;
            VKR_CHECK(vkrpc::destroy_command_pool(rpc, 65, h_fpool).ok);
            vkrpc::HandleRequest h_leaf;
            h_leaf.handle = ffence.fence;
            VKR_CHECK(vkrpc::destroy_fence(rpc, 66, h_leaf).ok);
            h_leaf.handle = fsem_acq.semaphore;
            VKR_CHECK(vkrpc::destroy_semaphore(rpc, 67, h_leaf).ok);
            h_leaf.handle = fsem_done.semaphore;
            VKR_CHECK(vkrpc::destroy_semaphore(rpc, 68, h_leaf).ok);
            VKR_CHECK(vkrpc::destroy_surface(rpc, 38, h_surf).ok);

            vkrpc::HandleRequest di;
            di.handle = ci.instance;
            VKR_CHECK(!vkrpc::destroy_instance(rpc, 39, di).ok); // still a live device
            vkrpc::HandleRequest dd;
            dd.handle = cd.device;
            VKR_CHECK(vkrpc::destroy_device(rpc, 40, dd).ok);
            VKR_CHECK(vkrpc::destroy_instance(rpc, 41, di).ok);
        }

        dp.reset(); // app EOF ends the worker session
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__,
                                 std::string("real rpc flow failed: ") + e.what());
    }

    server.join();
    sup.shutdown();
    return vkr::test::finish("integration_vulkan_real");
}
