// Minimal Vulkan RPC forwarding end to end. An app triggers a session, reconnects to the worker's
// data plane,
// and -- now past app_ack -- speaks the binary Vulkan RPC envelope to the worker:
// it negotiates Vulkan capabilities and learns the selected physical device. The
// worker's host-Vulkan side is the mock backend,
// so this runs headless on both platforms.
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
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace vkr;

namespace {

constexpr char kAppId[] = "integration-rpc-app";

// Drives the app side of LaunchSession; returns the SessionStarted.
protocol::SessionStarted launch_session(transport::MessageChannel& channel,
                                        const std::string& gpu_selector) {
    protocol::LaunchSession req;
    req.app_instance_id = kAppId;
    req.gpu_selector = gpu_selector;
    req.op_trace_enabled = true;
    channel.send(protocol::MessageType::LaunchSession, req.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body) || type != protocol::MessageType::SessionStarted) {
        throw std::runtime_error("expected session_started");
    }
    return protocol::SessionStarted::from_body(body);
}

// Connects to the worker's data plane, completes app_hello/app_ack, and returns
// the open connection ready for the binary RPC envelope.
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
        std::fprintf(stderr, "usage: integration_vulkan_rpc <worker-path>\n");
        return 2;
    }

    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = argv[1];
    cfg.heartbeat_timeout_ms = 5000;
    cfg.worker_interval_ms = 50;
    cfg.worker_count = 0; // run until killed

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

        const protocol::SessionStarted s = launch_session(channel, "high-performance");
        VKR_CHECK(!s.worker_id.empty());
        VKR_CHECK(s.data_plane_port > 0);
        VKR_CHECK(!s.op_trace_path.empty());

        // The worker should report the same device the supervisor selected.
        std::string reason;
        const auto sel = protocol::parse_selector("high-performance");
        const protocol::GpuDevice* chosen = protocol::select_device(devices, sel.selector, reason);
        VKR_CHECK(chosen != nullptr);

        auto dp = open_data_plane(s.data_plane_host, s.data_plane_port, s.app_token);
        vkrpc::RpcChannel rpc(*dp);

        // Negotiate at the supported level: granted exactly, with device identity.
        vkrpc::CapabilitiesRequest req;
        req.requested_api_major = 1;
        req.requested_api_minor = 3;
        const vkrpc::CapabilitiesResponse caps = vkrpc::negotiate_capabilities(rpc, 1, req);
        VKR_CHECK(caps.ok);
        VKR_CHECK_EQ(caps.negotiated_api_major, 1);
        VKR_CHECK_EQ(caps.negotiated_api_minor, 3);
        VKR_CHECK(!caps.device.device_name.empty());
        VKR_CHECK_EQ(caps.device.pipeline_specialization, 1u);
        if (chosen != nullptr) {
            VKR_CHECK_EQ(caps.device.device_name, chosen->name);
        }

        // Requesting a higher level is capped to what the worker supports.
        vkrpc::CapabilitiesRequest req_high;
        req_high.requested_api_major = 1;
        req_high.requested_api_minor = 4;
        const vkrpc::CapabilitiesResponse caps_high =
            vkrpc::negotiate_capabilities(rpc, 2, req_high);
        VKR_CHECK(caps_high.ok);
        VKR_CHECK_EQ(caps_high.negotiated_api_minor, 3);

        // An unknown opcode is rejected at the RPC layer but the stream stays
        // usable (response correlates by request_id).
        vkrpc::RpcMessage unknown;
        unknown.op = 4242;
        unknown.request_id = 3;
        rpc.send(unknown);
        vkrpc::RpcMessage unknown_resp;
        VKR_CHECK(rpc.recv(unknown_resp));
        VKR_CHECK_EQ(unknown_resp.request_id, static_cast<std::uint32_t>(3));
        VKR_CHECK(unknown_resp.status == static_cast<std::uint32_t>(vkrpc::RpcStatus::UnknownOp));

        // A request must carry status 0; a nonzero status is rejected as a bad
        // request, and the stream stays usable.
        vkrpc::RpcMessage bad;
        bad.op = static_cast<std::uint32_t>(vkrpc::RpcOp::NegotiateCapabilities);
        bad.request_id = 4;
        bad.status = 7; // requests must be 0
        bad.body = req.to_body().dump(0);
        rpc.send(bad);
        vkrpc::RpcMessage bad_resp;
        VKR_CHECK(rpc.recv(bad_resp));
        VKR_CHECK_EQ(bad_resp.request_id, static_cast<std::uint32_t>(4));
        VKR_CHECK(bad_resp.status == static_cast<std::uint32_t>(vkrpc::RpcStatus::BadRequest));

        // Instance/device lifecycle over RPC, with physical-device selection
        // enforced: create_instance -> enumerate -> create_device(selected).
        vkrpc::CreateInstanceRequest cir;
        cir.application_name = "integration-rpc-app";
        const vkrpc::CreateInstanceResponse ci = vkrpc::create_instance(rpc, 5, cir);
        VKR_CHECK(ci.ok);
        VKR_CHECK(ci.instance != 0);

        // create_device before enumerate is rejected even when guessing a handle:
        // the physical-device handle is only minted by enumeration.
        vkrpc::CreateDeviceRequest cdr_pre;
        cdr_pre.instance = ci.instance;
        cdr_pre.physical_device = ci.instance + 1;
        VKR_CHECK(!vkrpc::create_device(rpc, 6, cdr_pre).ok);

        vkrpc::EnumeratePhysicalDevicesRequest er;
        er.instance = ci.instance;
        const vkrpc::EnumeratePhysicalDevicesResponse en =
            vkrpc::enumerate_physical_devices(rpc, 7, er);
        VKR_CHECK(en.ok);
        VKR_CHECK_EQ(en.devices.size(), static_cast<std::size_t>(1));
        if (chosen != nullptr) {
            VKR_CHECK_EQ(en.devices.front().caps.device_name, chosen->name);
        }
        const std::uint64_t phys = en.devices.front().handle;

        vkrpc::CreateDeviceRequest cdr;
        cdr.instance = ci.instance;
        cdr.physical_device = phys;
        const vkrpc::CreateDeviceResponse cd = vkrpc::create_device(rpc, 8, cdr);
        VKR_CHECK(cd.ok);
        VKR_CHECK(cd.device != 0);

        // Both specialization raw opcodes cross the live serve loop. The mock rejects these
        // deliberately incomplete pipeline requests at the object-model layer; an unrecognized
        // opcode or malformed raw decoder path would instead make the helper throw on RPC status.
        vkrpc::SpecializationInfoDesc spec;
        spec.present = 1;
        spec.map_entries = {{5, 0, 4}};
        spec.data = std::string("\x2a\0\0\0", 4);
        vkrpc::CreateGraphicsPipelinesRequest raw_gp;
        raw_gp.device = cd.device;
        raw_gp.layout = 0xdead;
        raw_gp.render_pass = 0xbeef;
        vkrpc::ShaderStageDesc raw_stage;
        raw_stage.stage = 1;
        raw_stage.module = 0xcafe;
        raw_stage.entry = "main";
        raw_stage.specialization = spec;
        raw_gp.stages = {raw_stage};
        VKR_CHECK(!vkrpc::create_graphics_pipelines_raw(rpc, 80, raw_gp).ok);

        vkrpc::CreateComputePipelinesRequest raw_cp;
        raw_cp.device = cd.device;
        raw_cp.layout = 0xdead;
        raw_cp.shader_module = 0xcafe;
        raw_cp.entry_point = "main";
        raw_cp.specialization = spec;
        VKR_CHECK(!vkrpc::create_compute_pipelines_raw(rpc, 81, raw_cp).ok);

        // Selection enforced: a physical device not enumerated from this instance
        // is rejected (body-level !ok; the stream stays usable).
        vkrpc::CreateDeviceRequest cdr_bad;
        cdr_bad.instance = ci.instance;
        cdr_bad.physical_device = phys + 1;
        VKR_CHECK(!vkrpc::create_device(rpc, 9, cdr_bad).ok);

        // Device-child command objects: get the queue create_device reported (a
        // stable handle), create a command pool, allocate command buffers, free one.
        VKR_CHECK(cd.queue_count >= 1);
        vkrpc::GetDeviceQueueRequest gq;
        gq.device = cd.device;
        gq.queue_family_index = cd.queue_family_index;
        gq.queue_index = 0;
        const vkrpc::GetDeviceQueueResponse q = vkrpc::get_device_queue(rpc, 10, gq);
        VKR_CHECK(q.ok);
        VKR_CHECK(q.queue != 0);

        // A family the device never created is rejected over the wire (the stream
        // stays usable for the calls below).
        vkrpc::GetDeviceQueueRequest gq_bad = gq;
        gq_bad.queue_family_index = cd.queue_family_index + 1000;
        VKR_CHECK(!vkrpc::get_device_queue(rpc, 14, gq_bad).ok);

        vkrpc::CreateCommandPoolRequest cp;
        cp.device = cd.device;
        cp.queue_family_index = cd.queue_family_index;
        const vkrpc::CreateCommandPoolResponse pool = vkrpc::create_command_pool(rpc, 11, cp);
        VKR_CHECK(pool.ok);
        VKR_CHECK(pool.command_pool != 0);

        vkrpc::AllocateCommandBuffersRequest ab;
        ab.command_pool = pool.command_pool;
        ab.count = 2;
        const vkrpc::AllocateCommandBuffersResponse bufs =
            vkrpc::allocate_command_buffers(rpc, 12, ab);
        VKR_CHECK(bufs.ok);
        VKR_CHECK_EQ(bufs.command_buffers.size(), static_cast<std::size_t>(2));

        vkrpc::FreeCommandBuffersRequest fb;
        fb.command_pool = pool.command_pool;
        fb.command_buffers = {bufs.command_buffers[0]};
        VKR_CHECK(vkrpc::free_command_buffers(rpc, 13, fb).ok);

        // A batch repeating a live handle is rejected over the wire (atomic).
        vkrpc::FreeCommandBuffersRequest fb_dup;
        fb_dup.command_pool = pool.command_pool;
        fb_dup.command_buffers = {bufs.command_buffers[1], bufs.command_buffers[1]};
        VKR_CHECK(!vkrpc::free_command_buffers(rpc, 99, fb_dup).ok);

        // Device-child sync + memory objects (fence, semaphore, device memory).
        vkrpc::CreateFenceRequest cf;
        cf.device = cd.device;
        const vkrpc::CreateFenceResponse fence = vkrpc::create_fence(rpc, 20, cf);
        VKR_CHECK(fence.ok);
        VKR_CHECK(fence.fence != 0);

        vkrpc::CreateSemaphoreRequest cs;
        cs.device = cd.device;
        const vkrpc::CreateSemaphoreResponse sem = vkrpc::create_semaphore(rpc, 21, cs);
        VKR_CHECK(sem.ok);
        VKR_CHECK(sem.semaphore != 0);

        vkrpc::AllocateMemoryRequest am;
        am.device = cd.device;
        am.allocation_size = 4096;
        const vkrpc::AllocateMemoryResponse mem = vkrpc::allocate_memory(rpc, 22, am);
        VKR_CHECK(mem.ok);
        VKR_CHECK(mem.memory != 0);

        // A zero allocation size, and an out-of-range memory type index, are
        // rejected (the index is an unsigned Vulkan index).
        vkrpc::AllocateMemoryRequest am0 = am;
        am0.allocation_size = 0;
        VKR_CHECK(!vkrpc::allocate_memory(rpc, 23, am0).ok);
        vkrpc::AllocateMemoryRequest am_neg = am;
        am_neg.memory_type_index = -1;
        VKR_CHECK(!vkrpc::allocate_memory(rpc, 29, am_neg).ok);

        // A typed destroy rejects a wrong-kind handle (a fence handle to free_memory).
        vkrpc::HandleRequest h_fence;
        h_fence.handle = fence.fence;
        VKR_CHECK(!vkrpc::free_memory(rpc, 24, h_fence).ok);

        // Ordering over the wire: a device with a live pool can't be destroyed;
        // destroy the pool (cascading its remaining buffer) first.
        vkrpc::HandleRequest dd;
        dd.handle = cd.device;
        VKR_CHECK(!vkrpc::destroy_device(rpc, 14, dd).ok);
        vkrpc::HandleRequest dpool;
        dpool.handle = pool.command_pool;
        VKR_CHECK(vkrpc::destroy_command_pool(rpc, 15, dpool).ok);

        // With the pool gone the device still has live leaves (fence/semaphore/
        // memory), so it still can't be destroyed; destroy them first.
        VKR_CHECK(!vkrpc::destroy_device(rpc, 25, dd).ok);
        VKR_CHECK(vkrpc::destroy_fence(rpc, 26, h_fence).ok);
        vkrpc::HandleRequest h_sem;
        h_sem.handle = sem.semaphore;
        VKR_CHECK(vkrpc::destroy_semaphore(rpc, 27, h_sem).ok);
        vkrpc::HandleRequest h_mem;
        h_mem.handle = mem.memory;
        VKR_CHECK(vkrpc::free_memory(rpc, 28, h_mem).ok);

        // Presentation spine over the wire: a surface (instance child) + a swapchain
        // (device child targeting it), with destroy ordering.
        vkrpc::CreateSurfaceRequest csr;
        csr.instance = ci.instance;
        const vkrpc::CreateSurfaceResponse surf = vkrpc::create_surface(rpc, 30, csr);
        VKR_CHECK(surf.ok);
        VKR_CHECK(surf.surface != 0);

        // WSI capability queries over the RPC path (the exact flow the ICD uses before
        // create_swapchain): caps report the dynamic-extent sentinel + honest TRANSFER_DST;
        // formats/present-modes/support are non-empty/true.
        vkrpc::GetSurfaceCapabilitiesRequest scap_req;
        scap_req.physical_device = phys;
        scap_req.surface = surf.surface;
        const vkrpc::GetSurfaceCapabilitiesResponse scap =
            vkrpc::get_surface_capabilities(rpc, 45, scap_req);
        VKR_CHECK(scap.ok);
        VKR_CHECK_EQ(scap.current_extent_width, vkrpc::kDynamicExtentSentinel);
        VKR_CHECK((scap.supported_usage_flags &
                   static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst)) != 0);
        vkrpc::GetSurfaceFormatsRequest sfmt_req;
        sfmt_req.physical_device = phys;
        sfmt_req.surface = surf.surface;
        const vkrpc::GetSurfaceFormatsResponse sfmt = vkrpc::get_surface_formats(rpc, 46, sfmt_req);
        VKR_CHECK(sfmt.ok && !sfmt.formats.empty());
        vkrpc::GetSurfacePresentModesRequest spm_req;
        spm_req.physical_device = phys;
        spm_req.surface = surf.surface;
        const vkrpc::GetSurfacePresentModesResponse spm =
            vkrpc::get_surface_present_modes(rpc, 47, spm_req);
        VKR_CHECK(spm.ok && !spm.present_modes.empty());
        vkrpc::GetSurfaceSupportRequest ssup_req;
        ssup_req.physical_device = phys;
        ssup_req.queue_family_index = 0;
        ssup_req.surface = surf.surface;
        const vkrpc::GetSurfaceSupportResponse ssup = vkrpc::get_surface_support(rpc, 48, ssup_req);
        VKR_CHECK(ssup.ok && ssup.supported);

        // Choose the swapchain params from the queried values (as the ICD will), instead of
        // hardcoding them.
        vkrpc::CreateSwapchainRequest cscr;
        cscr.device = cd.device;
        cscr.surface = surf.surface;
        cscr.image_format = sfmt.formats.front().format;
        cscr.color_space = sfmt.formats.front().color_space;
        cscr.present_mode = spm.present_modes.front();
        cscr.width = 1280;
        cscr.height = 720;
        cscr.min_image_count = static_cast<int>(scap.min_image_count);
        cscr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
        const vkrpc::CreateSwapchainResponse swc = vkrpc::create_swapchain(rpc, 31, cscr);
        VKR_CHECK(swc.ok);
        VKR_CHECK(swc.swapchain != 0);
        // A surface with a live swapchain can't be destroyed; destroy the swapchain
        // first, then the surface.
        vkrpc::HandleRequest h_surf;
        h_surf.handle = surf.surface;
        VKR_CHECK(!vkrpc::destroy_surface(rpc, 32, h_surf).ok);
        vkrpc::HandleRequest h_swc;
        h_swc.handle = swc.swapchain;
        VKR_CHECK(vkrpc::destroy_swapchain(rpc, 33, h_swc).ok);
        VKR_CHECK(vkrpc::destroy_surface(rpc, 34, h_surf).ok);

        // An instance with a live device can't be destroyed; destroy the device
        // first, then the instance.
        vkrpc::HandleRequest di;
        di.handle = ci.instance;
        VKR_CHECK(!vkrpc::destroy_instance(rpc, 16, di).ok);
        VKR_CHECK(vkrpc::destroy_device(rpc, 17, dd).ok);
        VKR_CHECK(vkrpc::destroy_instance(rpc, 18, di).ok);

        // The session is still a single supervised worker while the app is up.
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(1));

        // App socket EOF closes the worker session (lifecycle invariant): close
        // the RPC connection and the supervised worker must become terminal.
        dp.reset();
        bool became_terminal = false;
        for (int i = 0; i < 250; ++i) {
            if (sup.active_count() == 0) {
                became_terminal = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        VKR_CHECK(became_terminal);

        // The opt-in diagnostic belongs to this session and is written by the worker at the
        // decoded request boundary. Verify the artifact itself, not only control-plane flag
        // forwarding: this flow allocates memory, so both the header and one decoded event must
        // be present after the worker has closed the file.
        if (!s.op_trace_path.empty()) {
            std::ifstream trace(s.op_trace_path);
            std::ostringstream contents;
            contents << trace.rdbuf();
            VKR_CHECK(trace.good() || trace.eof());
            VKR_CHECK(contents.str().find("\"event\":\"header\"") != std::string::npos);
            VKR_CHECK(contents.str().find("\"event\":\"allocate_memory\"") != std::string::npos);
            trace.close();
            std::error_code remove_error;
            std::filesystem::remove(s.op_trace_path, remove_error);
            VKR_CHECK(!remove_error);
        }
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("rpc flow failed: ") + e.what());
    }

    server.join();
    sup.shutdown();
    return vkr::test::finish("integration_vulkan_rpc");
}
