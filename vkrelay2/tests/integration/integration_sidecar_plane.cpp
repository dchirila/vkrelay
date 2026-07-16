// The sidecar control plane, end to end through the supervisor. With
// cfg.sidecar_plane = true, launch_session spawns a worker that opens BOTH a data-plane and a
// sidecar-plane listener (distinct ports), and SessionStarted reports the sidecar endpoint + token
// alongside the data-plane pair. A sidecar then connects to that endpoint, proves itself with the
// token (SidecarHello/SidecarAck), and exchanges a sidecar RPC (negotiate) over the binary RPC
// envelope. Asserts: the token gate (wrong token rejected without stranding the plane), structural
// session-mode separation (a Vulkan op number is unknown on the sidecar dispatcher), and that the
// two planes coexist on the one session-owned backend. Dual-platform (mock backend).
#include "windows/supervisor/worker_supervisor.hpp"

#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/sidecar/window_registry.hpp" // sidecar::PopupKind (cascade test)
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/vkrpc/rpc.hpp"
#include "tests/test_assert.hpp"
#include "windows/supervisor/display_snapshot_cache.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

using namespace vkr;

namespace {

constexpr char kAppId[] = "integration-sidecar-app";

// Connect to the sidecar plane and run SidecarHello(token); returns whether accepted.
bool sidecar_plane_accepts(const std::string& host, int port, const std::string& token) {
    try {
        auto conn = transport::tcp_connect(host, port);
        transport::MessageChannel channel(*conn);
        protocol::SidecarHello hello;
        hello.sidecar_token = token;
        channel.send(protocol::MessageType::SidecarHello, hello.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body) || type != protocol::MessageType::SidecarAck) {
            return false;
        }
        return protocol::SidecarAck::from_body(body).ok;
    } catch (const std::exception&) {
        return false;
    }
}

// Connect to the data plane and run AppHello(token); returns whether accepted.
bool data_plane_accepts(const std::string& host, int port, const std::string& token) {
    try {
        auto conn = transport::tcp_connect(host, port);
        transport::MessageChannel channel(*conn);
        protocol::AppHello hello;
        hello.app_token = token;
        channel.send(protocol::MessageType::AppHello, hello.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body) || type != protocol::MessageType::AppAck) {
            return false;
        }
        return protocol::AppAck::from_body(body).ok;
    } catch (const std::exception&) {
        return false;
    }
}

protocol::SessionStarted launch_session(transport::MessageChannel& channel,
                                        const std::string& gpu_selector,
                                        const std::string& display_snapshot_id) {
    protocol::LaunchSession req;
    req.app_instance_id = kAppId;
    req.gpu_selector = gpu_selector;
    req.display_snapshot_id = display_snapshot_id;
    channel.send(protocol::MessageType::LaunchSession, req.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body)) {
        throw std::runtime_error("control connection closed before session_started");
    }
    if (type == protocol::MessageType::Error) {
        const protocol::ErrorMsg error = protocol::ErrorMsg::from_body(body);
        throw std::runtime_error("launch rejected: " + error.code + " (" + error.message + ")");
    }
    if (type != protocol::MessageType::SessionStarted) {
        throw std::runtime_error("expected session_started");
    }
    return protocol::SessionStarted::from_body(body);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: integration_sidecar_plane <worker-path>\n");
        return 2;
    }

    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = argv[1];
    cfg.heartbeat_timeout_ms = 5000;
    cfg.worker_interval_ms = 50;
    cfg.worker_count = 0;
    cfg.sidecar_plane = true; // app sessions also get a sidecar plane

    supervisor::WorkerSupervisor sup(cfg);

    auto app_listener = transport::tcp_listen(0);
    const int app_port = app_listener->port();
    protocol::IdAllocator ids("sup-sidecar");
    const auto devices = protocol::probe_mocked();
    supervisor::DisplaySnapshotCache display_cache(
        "sup-sidecar", 8, [](const std::string& snapshot_id) {
            supervisor::DisplayLayoutProbeResult result;
            result.ok = true;
            result.layout.snapshot_id = snapshot_id;
            result.layout.virtual_bounds = {-100, -50, 800, 600};
            result.layout.primary_monitor_id = "only";
            display::MonitorDesc monitor;
            monitor.stable_id = "only";
            monitor.device_name = "DISPLAY1";
            monitor.bounds = result.layout.virtual_bounds;
            monitor.work = result.layout.virtual_bounds;
            monitor.dpi_x = 96;
            monitor.dpi_y = 96;
            monitor.primary = true;
            result.layout.monitors = {monitor};
            return result;
        });

    std::thread server([&] {
        try {
            auto conn = app_listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, ids, devices, &sup, "mock", {}, &display_cache);
        } catch (const std::exception&) {
        }
    });

    try {
        auto conn = transport::tcp_connect("127.0.0.1", app_port);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = kAppId;
        const protocol::ServerHello server_hello = transport::client_handshake(channel, hello);
        VKR_CHECK(server_hello.display_layout.status == display::LayoutDecodeStatus::Valid);
        // The additive legacy geometry/DPI fields are projections of this exact snapshot, not an
        // independent live work-area probe that could disagree with the new authority.
        VKR_CHECK_EQ(server_hello.host_display.work_w, 800);
        VKR_CHECK_EQ(server_hello.host_display.work_h, 600);
        VKR_CHECK_EQ(server_hello.host_display.dpi, 96);

        const protocol::SessionStarted s =
            launch_session(channel, "auto", server_hello.display_layout.layout.snapshot_id);
        // Both planes reported, on distinct ports, with a sidecar token.
        VKR_CHECK(s.data_plane_port > 0);
        VKR_CHECK(s.sidecar_plane_port > 0);
        VKR_CHECK(s.data_plane_port != s.sidecar_plane_port);
        VKR_CHECK(!s.sidecar_token.empty());
        VKR_CHECK_EQ(s.sidecar_plane_host, std::string("127.0.0.1"));
        VKR_CHECK_EQ(s.display_snapshot_id, server_hello.display_layout.layout.snapshot_id);

        // The token gate: a wrong token is rejected WITHOUT stranding the plane (the real sidecar
        // can still connect afterward).
        VKR_CHECK(
            !sidecar_plane_accepts(s.sidecar_plane_host, s.sidecar_plane_port, "wrong-token"));
        VKR_CHECK(
            sidecar_plane_accepts(s.sidecar_plane_host, s.sidecar_plane_port, s.sidecar_token));

        // Full sidecar flow on one connection: handshake -> binary RPC -> negotiate, then the
        // session-mode gate (a Vulkan op number is unknown on the sidecar dispatcher).
        {
            auto sc = transport::tcp_connect(s.sidecar_plane_host, s.sidecar_plane_port);
            transport::MessageChannel sc_channel(*sc);
            protocol::SidecarHello sh;
            sh.sidecar_token = s.sidecar_token;
            sc_channel.send(protocol::MessageType::SidecarHello, sh.to_body());
            protocol::MessageType type = protocol::MessageType::Unknown;
            json::Value body;
            VKR_CHECK(sc_channel.recv(type, body) && type == protocol::MessageType::SidecarAck);
            const protocol::SidecarAck sidecar_ack = protocol::SidecarAck::from_body(body);
            VKR_CHECK(sidecar_ack.ok);
            VKR_CHECK(sidecar_ack.display_layout.status == display::LayoutDecodeStatus::Valid);
            VKR_CHECK_EQ(sidecar_ack.display_layout.layout.snapshot_id, s.display_snapshot_id);
            VKR_CHECK(sidecar_ack.display_layout.layout.virtual_bounds ==
                      server_hello.display_layout.layout.virtual_bounds);

            vkrpc::RpcChannel rpc(*sc);
            vkrpc::RpcMessage req;
            req.op = static_cast<std::uint32_t>(sidecar::SidecarOp::Negotiate);
            req.request_id = 1;
            req.body = sidecar::SidecarNegotiateRequest{}.to_body().dump(0);
            rpc.send(req);
            vkrpc::RpcMessage resp;
            VKR_CHECK(rpc.recv(resp));
            VKR_CHECK_EQ(resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
            json::Value rbody;
            std::string err;
            VKR_CHECK(json::Value::try_parse(resp.body, rbody, err));
            const sidecar::SidecarNegotiateResponse nresp =
                sidecar::SidecarNegotiateResponse::from_body(rbody);
            VKR_CHECK(nresp.ok);
            VKR_CHECK_EQ(nresp.protocol_version, sidecar::kSidecarProtocolVersion);

            // The worker has the full pinned snapshot from its launch environment. A sidecar that
            // names it but reports a different observed root is rejected before readiness.
            vkrpc::RpcMessage bad_rdy;
            bad_rdy.op = static_cast<std::uint32_t>(sidecar::SidecarOp::Ready);
            bad_rdy.request_id = 2;
            sidecar::SidecarReadyRequest bad_rreq;
            bad_rreq.scan_generation = 1;
            bad_rreq.root_width = 799;
            bad_rreq.root_height = 600;
            bad_rreq.display_snapshot_id = s.display_snapshot_id;
            bad_rdy.body = bad_rreq.to_body().dump(0);
            rpc.send(bad_rdy);
            vkrpc::RpcMessage bad_rdy_resp;
            VKR_CHECK(rpc.recv(bad_rdy_resp));
            json::Value bad_rdy_body;
            VKR_CHECK(json::Value::try_parse(bad_rdy_resp.body, bad_rdy_body, err));
            VKR_CHECK(!sidecar::SidecarReadyResponse::from_body(bad_rdy_body).ok);

            // The readiness-barrier RPC is accepted over the real plane.
            vkrpc::RpcMessage rdy;
            rdy.op = static_cast<std::uint32_t>(sidecar::SidecarOp::Ready);
            rdy.request_id = 3;
            sidecar::SidecarReadyRequest rreq;
            rreq.scan_generation = 1;
            rreq.has_xcomposite = true;
            rreq.root_width = 800;
            rreq.root_height = 600;
            rreq.display_snapshot_id = s.display_snapshot_id;
            rdy.body = rreq.to_body().dump(0);
            rpc.send(rdy);
            vkrpc::RpcMessage rdy_resp;
            VKR_CHECK(rpc.recv(rdy_resp));
            VKR_CHECK_EQ(rdy_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
            json::Value rdy_body;
            VKR_CHECK(json::Value::try_parse(rdy_resp.body, rdy_body, err));
            VKR_CHECK(sidecar::SidecarReadyResponse::from_body(rdy_body).ok);

            // The toplevel-registry lifecycle RPCs round-trip over the real plane: a
            // register of a not-yet-surfaced toplevel creates a placeholder representation; the
            // unregister drops it. The mock worker runs the SAME WindowRegistry as the real one.
            {
                vkrpc::RpcMessage rt;
                rt.op = static_cast<std::uint32_t>(sidecar::SidecarOp::RegisterToplevel);
                rt.request_id = 4;
                sidecar::SidecarRegisterToplevelRequest treq;
                treq.xid = 0x4242;
                treq.generation = 1;
                treq.role = "toplevel";
                treq.width = 320;
                treq.height = 240;
                rt.body = treq.to_body().dump(0);
                rpc.send(rt);
                vkrpc::RpcMessage rt_resp;
                VKR_CHECK(rpc.recv(rt_resp));
                VKR_CHECK_EQ(rt_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                json::Value rt_body;
                VKR_CHECK(json::Value::try_parse(rt_resp.body, rt_body, err));
                const sidecar::SidecarToplevelResponse rt_dec =
                    sidecar::SidecarToplevelResponse::from_body(rt_body);
                VKR_CHECK(rt_dec.ok && rt_dec.applied);
                VKR_CHECK_EQ(rt_dec.representation, std::string("placeholder"));

                vkrpc::RpcMessage ut;
                ut.op = static_cast<std::uint32_t>(sidecar::SidecarOp::UnregisterToplevel);
                ut.request_id = 5;
                sidecar::SidecarUnregisterToplevelRequest ureq;
                ureq.xid = 0x4242;
                ureq.generation = 2;
                ut.body = ureq.to_body().dump(0);
                rpc.send(ut);
                vkrpc::RpcMessage ut_resp;
                VKR_CHECK(rpc.recv(ut_resp));
                VKR_CHECK_EQ(ut_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                json::Value ut_body;
                VKR_CHECK(json::Value::try_parse(ut_resp.body, ut_body, err));
                const sidecar::SidecarToplevelResponse ut_dec =
                    sidecar::SidecarToplevelResponse::from_body(ut_body);
                VKR_CHECK(ut_dec.ok && ut_dec.applied);
                VKR_CHECK_EQ(ut_dec.representation, std::string("none"));
            }

            // Chrome pixels over the real plane: register a placeholder, PaintChrome a
            // known solid color (the BINARY-bodied op), then DebugChromeState (the worker-visible
            // structured query) -> the worker sampled the painted pixel from its DIB/store.
            {
                vkrpc::RpcMessage rt;
                rt.op = static_cast<std::uint32_t>(sidecar::SidecarOp::RegisterToplevel);
                rt.request_id = 6;
                sidecar::SidecarRegisterToplevelRequest treq;
                treq.xid = 0x4343;
                treq.generation = 1;
                treq.width = 8;
                treq.height = 4;
                rt.body = treq.to_body().dump(0);
                rpc.send(rt);
                vkrpc::RpcMessage rt_resp;
                VKR_CHECK(rpc.recv(rt_resp));

                // PaintChrome: a solid 8x4 BGRA color (B=0x11 G=0x22 R=0x33 A=0xFF), the whole
                // source.
                sidecar::SidecarPaintChromeRequest paint;
                paint.xid = 0x4343;
                paint.lifecycle_generation = 1;
                paint.seq = 1;
                paint.src_w = 8;
                paint.src_h = 4;
                paint.dirty_w = 8;
                paint.dirty_h = 4;
                paint.stride = 32;
                paint.pixels.resize(32 * 4);
                for (std::size_t i = 0; i < paint.pixels.size(); i += 4) {
                    paint.pixels[i + 0] = static_cast<char>(0x11);
                    paint.pixels[i + 1] = static_cast<char>(0x22);
                    paint.pixels[i + 2] = static_cast<char>(0x33);
                    paint.pixels[i + 3] = static_cast<char>(0xFF);
                }
                vkrpc::RpcMessage pc;
                pc.op = static_cast<std::uint32_t>(sidecar::SidecarOp::PaintChrome);
                pc.request_id = 7;
                pc.body = paint.to_wire(); // BINARY body
                rpc.send(pc);
                vkrpc::RpcMessage pc_resp;
                VKR_CHECK(rpc.recv(pc_resp));
                VKR_CHECK_EQ(pc_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                json::Value pc_body;
                VKR_CHECK(json::Value::try_parse(pc_resp.body, pc_body, err));
                const sidecar::SidecarPaintResponse pc_dec =
                    sidecar::SidecarPaintResponse::from_body(pc_body);
                VKR_CHECK(pc_dec.ok && pc_dec.applied && pc_dec.shown);
                VKR_CHECK_EQ(pc_dec.representation, std::string("placeholder"));

                // DebugChromeState samples (3,2) -> the painted color, proven over the wire.
                sidecar::SidecarDebugChromeStateRequest dq;
                dq.xid = 0x4343;
                dq.sample_x = 3;
                dq.sample_y = 2;
                vkrpc::RpcMessage ds;
                ds.op = static_cast<std::uint32_t>(sidecar::SidecarOp::DebugChromeState);
                ds.request_id = 8;
                ds.body = dq.to_body().dump(0);
                rpc.send(ds);
                vkrpc::RpcMessage ds_resp;
                VKR_CHECK(rpc.recv(ds_resp));
                VKR_CHECK_EQ(ds_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                json::Value ds_body;
                VKR_CHECK(json::Value::try_parse(ds_resp.body, ds_body, err));
                const sidecar::SidecarDebugChromeStateResponse ds_dec =
                    sidecar::SidecarDebugChromeStateResponse::from_body(ds_body);
                VKR_CHECK(ds_dec.ok && ds_dec.shown && ds_dec.has_pixel);
                VKR_CHECK_EQ(ds_dec.pixel_bgra, static_cast<std::uint32_t>(0xFF332211u));

                // A truncated PaintChrome body is rejected at the dispatcher (BadRequest), not
                // served.
                vkrpc::RpcMessage bad;
                bad.op = static_cast<std::uint32_t>(sidecar::SidecarOp::PaintChrome);
                bad.request_id = 9;
                bad.body = paint.to_wire().substr(0, paint.to_wire().size() - 1);
                rpc.send(bad);
                vkrpc::RpcMessage bad_resp;
                VKR_CHECK(rpc.recv(bad_resp));
                VKR_CHECK_EQ(bad_resp.status,
                             static_cast<std::uint32_t>(vkrpc::RpcStatus::BadRequest));
            }

            // Observability over the real plane: DebugEnumWindows lists the registry,
            // so the placeholder 0x4343 painted just above appears with its representation/shown/
            // epoch -- proving the dispatcher serves the enum end to end, not just the body codec.
            {
                sidecar::SidecarDebugEnumWindowsRequest ereq;
                vkrpc::RpcMessage eq;
                eq.op = static_cast<std::uint32_t>(sidecar::SidecarOp::DebugEnumWindows);
                eq.request_id = 20;
                eq.body = ereq.to_body().dump(0);
                rpc.send(eq);
                vkrpc::RpcMessage eq_resp;
                VKR_CHECK(rpc.recv(eq_resp));
                VKR_CHECK_EQ(eq_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                json::Value eq_body;
                VKR_CHECK(json::Value::try_parse(eq_resp.body, eq_body, err));
                const sidecar::SidecarDebugEnumWindowsResponse eq_dec =
                    sidecar::SidecarDebugEnumWindowsResponse::from_body(eq_body);
                VKR_CHECK(eq_dec.ok);
                bool found = false;
                for (const auto& w : eq_dec.windows) {
                    if (w.xid == 0x4343u) {
                        found = true;
                        VKR_CHECK_EQ(w.representation, std::string("placeholder"));
                        VKR_CHECK(w.toplevel_registered && w.shown && w.epoch != 0);
                    }
                }
                VKR_CHECK(found);
            }

            // Source-layer capture over the real plane: DebugCaptureWindow returns
            // the BINARY-bodied response; the painted 0x4343 chrome decodes to the actual pixels --
            // proving the reply_wire path + the response decoder end to end.
            {
                sidecar::SidecarDebugCaptureWindowRequest creq;
                creq.xid = 0x4343;
                creq.layer = sidecar::kCaptureLayerChrome;
                vkrpc::RpcMessage cap;
                cap.op = static_cast<std::uint32_t>(sidecar::SidecarOp::DebugCaptureWindow);
                cap.request_id = 21;
                cap.body = creq.to_body().dump(0);
                rpc.send(cap);
                vkrpc::RpcMessage cap_resp;
                VKR_CHECK(rpc.recv(cap_resp));
                VKR_CHECK_EQ(cap_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                std::string cerr;
                const sidecar::SidecarDebugCaptureWindowResponse cdec =
                    sidecar::SidecarDebugCaptureWindowResponse::from_wire(cap_resp.body, cerr);
                VKR_CHECK(cerr.empty());
                VKR_CHECK(cdec.ok && cdec.status == "ok");
                VKR_CHECK_EQ(cdec.width, static_cast<std::uint32_t>(8));
                VKR_CHECK_EQ(cdec.height, static_cast<std::uint32_t>(4));
                VKR_CHECK_EQ(cdec.pixels.size(), static_cast<std::size_t>(8 * 4 * 4));
                VKR_CHECK_EQ(static_cast<unsigned char>(cdec.pixels[0]),
                             static_cast<unsigned char>(0x11));

                // A non-OK result (mismatch) also rides the binary frame with an empty tail.
                sidecar::SidecarDebugCaptureWindowRequest mreq;
                mreq.xid = 0x4343;
                mreq.layer = sidecar::kCaptureLayerChrome;
                mreq.expected_lifecycle_generation = 999;
                vkrpc::RpcMessage mm;
                mm.op = static_cast<std::uint32_t>(sidecar::SidecarOp::DebugCaptureWindow);
                mm.request_id = 22;
                mm.body = mreq.to_body().dump(0);
                rpc.send(mm);
                vkrpc::RpcMessage mm_resp;
                VKR_CHECK(rpc.recv(mm_resp));
                VKR_CHECK_EQ(mm_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                std::string merr;
                const sidecar::SidecarDebugCaptureWindowResponse mdec =
                    sidecar::SidecarDebugCaptureWindowResponse::from_wire(mm_resp.body, merr);
                VKR_CHECK(merr.empty() && !mdec.ok && mdec.status == "mismatch");
                VKR_CHECK(mdec.pixels.empty());
            }

            // Popup owner-teardown cascade over the real plane: register an owner
            // toplevel + a classified popup owned by it, prove the linkage via DebugEnumWindows,
            // then UNREGISTER THE OWNER -> both the owner AND the orphan popup are gone (the
            // registry's owner-teardown cascade dropped the popup; the worker reaped its host) --
            // proving the cascade end to end over the wire, not just in-process.
            {
                auto reg_toplevel = [&](std::uint32_t request_id,
                                        const sidecar::SidecarRegisterToplevelRequest& treq) {
                    vkrpc::RpcMessage rt;
                    rt.op = static_cast<std::uint32_t>(sidecar::SidecarOp::RegisterToplevel);
                    rt.request_id = request_id;
                    rt.body = treq.to_body().dump(0);
                    rpc.send(rt);
                    vkrpc::RpcMessage rt_resp;
                    VKR_CHECK(rpc.recv(rt_resp));
                    VKR_CHECK_EQ(rt_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));
                    json::Value rt_body;
                    VKR_CHECK(json::Value::try_parse(rt_resp.body, rt_body, err));
                    return sidecar::SidecarToplevelResponse::from_body(rt_body);
                };
                // Returns whether `xid` is enumerated, and (out) its is_popup + owner_xid.
                auto enum_find = [&](std::uint32_t request_id, std::uint64_t xid, bool& is_popup,
                                     std::uint64_t& owner) {
                    sidecar::SidecarDebugEnumWindowsRequest ereq;
                    vkrpc::RpcMessage eq;
                    eq.op = static_cast<std::uint32_t>(sidecar::SidecarOp::DebugEnumWindows);
                    eq.request_id = request_id;
                    eq.body = ereq.to_body().dump(0);
                    rpc.send(eq);
                    vkrpc::RpcMessage eq_resp;
                    VKR_CHECK(rpc.recv(eq_resp));
                    json::Value eq_body;
                    VKR_CHECK(json::Value::try_parse(eq_resp.body, eq_body, err));
                    const sidecar::SidecarDebugEnumWindowsResponse dec =
                        sidecar::SidecarDebugEnumWindowsResponse::from_body(eq_body);
                    for (const auto& w : dec.windows) {
                        if (w.xid == xid) {
                            is_popup = w.is_popup;
                            owner = w.owner_xid;
                            return true;
                        }
                    }
                    return false;
                };

                sidecar::SidecarRegisterToplevelRequest owner;
                owner.xid = 0x5050;
                owner.generation = 1;
                owner.role = "toplevel";
                owner.width = 400;
                owner.height = 300;
                VKR_CHECK(reg_toplevel(30, owner).applied);

                sidecar::SidecarRegisterToplevelRequest popup;
                popup.xid = 0x5051;
                popup.generation = 1;
                popup.override_redirect = true;
                popup.is_popup = true;
                popup.owner_xid = 0x5050;
                popup.popup_kind = static_cast<std::uint32_t>(sidecar::PopupKind::Menu);
                popup.width = 120;
                popup.height = 160;
                const sidecar::SidecarToplevelResponse popup_resp = reg_toplevel(31, popup);
                VKR_CHECK(popup_resp.applied);
                VKR_CHECK_EQ(popup_resp.representation, std::string("placeholder"));

                bool pis = false;
                std::uint64_t pown = 0;
                VKR_CHECK(enum_find(32, 0x5051, pis, pown)); // popup present...
                VKR_CHECK(pis && pown == 0x5050u);           // ...owned by the toplevel
                bool obit = false;
                std::uint64_t oown = 0;
                VKR_CHECK(enum_find(33, 0x5050, obit, oown)); // owner present, not a popup
                VKR_CHECK(!obit);

                vkrpc::RpcMessage ut;
                ut.op = static_cast<std::uint32_t>(sidecar::SidecarOp::UnregisterToplevel);
                ut.request_id = 34;
                sidecar::SidecarUnregisterToplevelRequest ureq;
                ureq.xid = 0x5050; // unregister the OWNER -> cascade drops the popup
                ureq.generation = 2;
                ut.body = ureq.to_body().dump(0);
                rpc.send(ut);
                vkrpc::RpcMessage ut_resp;
                VKR_CHECK(rpc.recv(ut_resp));
                VKR_CHECK_EQ(ut_resp.status, static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok));

                bool gone_b = false;
                std::uint64_t gone_o = 0;
                VKR_CHECK(!enum_find(35, 0x5050, gone_b, gone_o)); // owner gone
                VKR_CHECK(
                    !enum_find(36, 0x5051, gone_b, gone_o)); // popup cascaded away over the wire
            }

            // Structural session-mode separation: a Vulkan opcode on the sidecar plane is unknown.
            // Cover BOTH the first Vulkan op (NegotiateCapabilities = 1, which must NOT alias the
            // sidecar negotiate op now that the sidecar range is disjoint) and CreateInstance (2).
            for (const vkrpc::RpcOp vk_op :
                 {vkrpc::RpcOp::NegotiateCapabilities, vkrpc::RpcOp::CreateInstance}) {
                vkrpc::RpcMessage vk;
                vk.op = static_cast<std::uint32_t>(vk_op);
                vk.request_id = 10 + static_cast<std::uint32_t>(vk_op);
                vk.body = "{}";
                rpc.send(vk);
                vkrpc::RpcMessage vk_resp;
                VKR_CHECK(rpc.recv(vk_resp));
                VKR_CHECK_EQ(vk_resp.status,
                             static_cast<std::uint32_t>(vkrpc::RpcStatus::UnknownOp));
            }
        }

        // The two planes coexist on the one session-owned backend: the data plane still accepts the
        // app token. (This connects + disconnects the app, which ends the session, so it is last.)
        VKR_CHECK(data_plane_accepts(s.data_plane_host, s.data_plane_port, s.app_token));
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__,
                                 std::string("sidecar flow failed: ") + e.what());
    }

    server.join();
    sup.shutdown();
    return vkr::test::finish("integration_sidecar_plane");
}
