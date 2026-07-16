// vkrelay2 geometry-state query (boundary-smoke helper).
//
// A sidecar-plane RPC client (NO xcb, NO Vulkan): it proves a guest toplevel's LIVE host geometry
// from OUTSIDE the worker, the worker-visible way -- it connects to the worker's sidecar plane
// (SidecarHello + token, like the real sidecar), issues a DebugEnumWindows query with
// include_actual=true, and exits 0 iff the enumeration shows the xid with has_actual=true, the
// ACTUAL host CLIENT origin (mapped back to X-root coords) == --expect-x/--expect-y, and
// last_host_apply_seq
// != 0 (a move was applied). So the boundary smoke asserts the worker converged the host window
// to the sidecar-authored position over the wire, not by scraping a log. The worker serves one
// sidecar connection at a time, so the smoke runs this AFTER the capturing sidecar has been
// stopped.
#include "common/protocol/messages.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

bool parse_host_port(const std::string& text, std::string& host, int& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    port = std::atoi(text.substr(colon + 1).c_str());
    return port > 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string connect;
    std::string token;
    std::uint64_t xid = 0;
    bool have_x = false, have_y = false;
    int expect_x = 0, expect_y = 0;
    // Optional extent gate: when both are provided, also assert the actual client extent.
    bool have_w = false, have_h = false;
    int expect_w = 0, expect_h = 0;
    // Optional z-order gate: when provided, also assert the reported last z-order intent.
    bool have_z = false;
    int expect_z = 0;
    // Optional gates: the host-observed visibility, and the representation epoch (the hard
    // hide/show gate -- the SAME epoch across a hide/show proves the representation SURVIVED the
    // hide rather than being torn down + rebuilt).
    bool have_visible = false;
    int expect_visible = 0;
    // Optional gate: the host-observed ICONIC state (a minimized host is IsIconic AND still
    // IsWindowVisible -- the pending-iconify guard kept it Iconic, NOT downgraded to Hidden).
    bool have_iconic = false;
    int expect_iconic = 0;
    bool have_epoch = false;
    std::uint64_t expect_epoch = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "geometry-query: missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--connect") {
            connect = next("--connect");
        } else if (arg == "--sidecar-token") {
            token = next("--sidecar-token");
        } else if (arg == "--xid") {
            xid = std::strtoull(next("--xid"), nullptr, 0);
        } else if (arg == "--expect-x") {
            expect_x = std::atoi(next("--expect-x"));
            have_x = true;
        } else if (arg == "--expect-y") {
            expect_y = std::atoi(next("--expect-y"));
            have_y = true;
        } else if (arg == "--expect-w") {
            expect_w = std::atoi(next("--expect-w"));
            have_w = true;
        } else if (arg == "--expect-h") {
            expect_h = std::atoi(next("--expect-h"));
            have_h = true;
        } else if (arg == "--expect-z") {
            expect_z = std::atoi(next("--expect-z"));
            have_z = true;
        } else if (arg == "--expect-visible") {
            expect_visible = std::atoi(next("--expect-visible"));
            have_visible = true;
        } else if (arg == "--expect-iconic") {
            expect_iconic = std::atoi(next("--expect-iconic"));
            have_iconic = true;
        } else if (arg == "--expect-epoch") {
            expect_epoch = std::strtoull(next("--expect-epoch"), nullptr, 0);
            have_epoch = true;
        } else {
            std::fprintf(stderr, "geometry-query: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (xid == 0 || !have_x || !have_y) {
        std::fprintf(stderr, "geometry-query: --xid, --expect-x and --expect-y are required\n");
        return 2;
    }

    std::string host;
    int port = 0;
    if (!parse_host_port(connect, host, port)) {
        std::fprintf(stderr, "geometry-query: invalid --connect %s\n", connect.c_str());
        return 2;
    }

    std::unique_ptr<vkr::transport::Connection> conn;
    try {
        conn = vkr::transport::tcp_connect(host, port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "geometry-query: cannot reach sidecar plane %s: %s\n", connect.c_str(),
                     e.what());
        return 3;
    }
    vkr::transport::MessageChannel channel(*conn);
    vkr::protocol::SidecarHello hello;
    hello.sidecar_token = token;
    channel.send(vkr::protocol::MessageType::SidecarHello, hello.to_body());
    vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
    vkr::json::Value body;
    if (!channel.recv(type, body) || type != vkr::protocol::MessageType::SidecarAck ||
        !vkr::protocol::SidecarAck::from_body(body).ok) {
        std::fprintf(stderr, "geometry-query: sidecar handshake rejected\n");
        return 3;
    }

    vkr::vkrpc::RpcChannel rpc(*conn);
    vkr::vkrpc::RpcMessage msg;
    msg.op = static_cast<std::uint32_t>(vkr::sidecar::SidecarOp::DebugEnumWindows);
    msg.request_id = 1;
    vkr::sidecar::SidecarDebugEnumWindowsRequest req;
    req.include_actual = true; // ask the worker to read the ACTUAL host geometry
    msg.body = req.to_body().dump(0);
    rpc.send(msg);
    vkr::vkrpc::RpcMessage resp;
    if (!rpc.recv(resp) || resp.status != 0) {
        std::fprintf(stderr, "geometry-query: DebugEnumWindows rpc failed (status %u)\n",
                     resp.status);
        return 3;
    }
    vkr::json::Value rbody;
    std::string err;
    if (!vkr::json::Value::try_parse(resp.body, rbody, err)) {
        std::fprintf(stderr, "geometry-query: bad response body: %s\n", err.c_str());
        return 3;
    }
    const vkr::sidecar::SidecarDebugEnumWindowsResponse r =
        vkr::sidecar::SidecarDebugEnumWindowsResponse::from_body(rbody);

    const vkr::sidecar::SidecarWindowInfo* w = nullptr;
    for (const auto& info : r.windows) {
        if (info.xid == xid) {
            w = &info;
            break;
        }
    }
    std::printf("GEOMETRY-QUERY: windows=%zu found=%d\n", r.windows.size(), w != nullptr ? 1 : 0);
    if (w != nullptr) {
        std::printf(
            "GEOMETRY-QUERY: xid=%llu rep=%s has_actual=%d actual=%d,%d seq=%llu clamped=%d "
            "z=%u vis=%u host_visible=%d host_iconic=%d epoch=%llu\n",
            static_cast<unsigned long long>(w->xid), w->representation.c_str(),
            w->has_actual ? 1 : 0, w->actual_x, w->actual_y,
            static_cast<unsigned long long>(w->last_host_apply_seq), w->clamped ? 1 : 0, w->z_order,
            w->visibility_state, w->host_visible ? 1 : 0, w->host_iconic ? 1 : 0,
            static_cast<unsigned long long>(w->epoch));
    }
    std::fflush(stdout);

    if (w == nullptr) {
        std::printf("GEOMETRY-QUERY: FAIL (xid not represented)\n");
        return 1;
    }
    if (!w->has_actual) {
        std::printf("GEOMETRY-QUERY: FAIL (no actual geometry -- no host move applied)\n");
        return 1;
    }
    if (w->last_host_apply_seq == 0) {
        std::printf("GEOMETRY-QUERY: FAIL (last_host_apply_seq == 0 -- move never applied)\n");
        return 1;
    }
    if (w->actual_x != expect_x || w->actual_y != expect_y) {
        std::printf("GEOMETRY-QUERY: FAIL (actual %d,%d != expected %d,%d)\n", w->actual_x,
                    w->actual_y, expect_x, expect_y);
        return 1;
    }
    if (have_w && have_h &&
        (w->actual_width != static_cast<std::uint32_t>(expect_w) ||
         w->actual_height != static_cast<std::uint32_t>(expect_h))) {
        std::printf("GEOMETRY-QUERY: FAIL (actual extent %ux%u != expected %dx%d)\n",
                    w->actual_width, w->actual_height, expect_w, expect_h);
        return 1;
    }
    if (have_z && w->z_order != static_cast<std::uint32_t>(expect_z)) {
        std::printf("GEOMETRY-QUERY: FAIL (z_order %u != expected %d)\n", w->z_order, expect_z);
        return 1;
    }
    if (have_visible && (w->host_visible ? 1 : 0) != expect_visible) {
        std::printf("GEOMETRY-QUERY: FAIL (host_visible %d != expected %d)\n",
                    w->host_visible ? 1 : 0, expect_visible);
        return 1;
    }
    if (have_iconic && (w->host_iconic ? 1 : 0) != expect_iconic) {
        std::printf("GEOMETRY-QUERY: FAIL (host_iconic %d != expected %d)\n",
                    w->host_iconic ? 1 : 0, expect_iconic);
        return 1;
    }
    if (have_epoch && w->epoch != expect_epoch) {
        std::printf("GEOMETRY-QUERY: FAIL (epoch %llu != expected %llu -- representation did not "
                    "survive the hide/show)\n",
                    static_cast<unsigned long long>(w->epoch),
                    static_cast<unsigned long long>(expect_epoch));
        return 1;
    }
    if (have_w && have_h) {
        std::printf("GEOMETRY-QUERY: PASS (host converged to %d,%d @ %dx%d)\n", expect_x, expect_y,
                    expect_w, expect_h);
    } else {
        std::printf("GEOMETRY-QUERY: PASS (host converged to %d,%d)\n", expect_x, expect_y);
    }
    return 0;
}
