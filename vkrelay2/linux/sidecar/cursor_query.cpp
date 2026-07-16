// vkrelay2 cursor-state query (boundary-smoke helper).
//
// A sidecar-plane RPC client (NO xcb, NO Vulkan): it proves a guest toplevel's cursor from OUTSIDE
// the worker, the worker-visible way -- it connects to the worker's sidecar plane (SidecarHello +
// token, like the real sidecar), issues the DebugCursorState query for an XID + a sample pixel, and
// exits 0 iff the worker reports it built a cursor AND the sampled pixel matches --expect-bgra. So
// the boundary smoke asserts the worker's built HCURSOR over the wire, not by scraping a log. The
// worker serves one sidecar connection at a time, so the smoke runs this AFTER the capturing
// sidecar has shipped the cursor and been stopped (freeing the plane).
#include "common/protocol/messages.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    int sample_x = 0;
    int sample_y = 0;
    std::uint32_t expect_bgra = 0;
    bool have_expect = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cursor-query: missing value for %s\n", what);
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
        } else if (arg == "--sample-x") {
            sample_x = std::atoi(next("--sample-x"));
        } else if (arg == "--sample-y") {
            sample_y = std::atoi(next("--sample-y"));
        } else if (arg == "--expect-bgra") {
            expect_bgra =
                static_cast<std::uint32_t>(std::strtoull(next("--expect-bgra"), nullptr, 0));
            have_expect = true;
        } else {
            std::fprintf(stderr, "cursor-query: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    std::string host;
    int port = 0;
    if (!parse_host_port(connect, host, port)) {
        std::fprintf(stderr, "cursor-query: invalid --connect %s\n", connect.c_str());
        return 2;
    }

    std::unique_ptr<vkr::transport::Connection> conn;
    try {
        conn = vkr::transport::tcp_connect(host, port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cursor-query: cannot reach sidecar plane %s: %s\n", connect.c_str(),
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
        std::fprintf(stderr, "cursor-query: sidecar handshake rejected\n");
        return 3;
    }

    vkr::vkrpc::RpcChannel rpc(*conn);
    vkr::sidecar::SidecarDebugCursorStateRequest req;
    req.xid = xid;
    req.sample_x = sample_x;
    req.sample_y = sample_y;
    vkr::vkrpc::RpcMessage msg;
    msg.op = static_cast<std::uint32_t>(vkr::sidecar::SidecarOp::DebugCursorState);
    msg.request_id = 1;
    msg.body = req.to_body().dump(0);
    rpc.send(msg);
    vkr::vkrpc::RpcMessage resp;
    if (!rpc.recv(resp) || resp.status != 0) {
        std::fprintf(stderr, "cursor-query: DebugCursorState rpc failed (status %u)\n",
                     resp.status);
        return 3;
    }
    vkr::json::Value rbody;
    std::string err;
    if (!vkr::json::Value::try_parse(resp.body, rbody, err)) {
        std::fprintf(stderr, "cursor-query: bad response body: %s\n", err.c_str());
        return 3;
    }
    const vkr::sidecar::SidecarDebugCursorStateResponse r =
        vkr::sidecar::SidecarDebugCursorStateResponse::from_body(rbody);
    std::printf("CURSOR-QUERY: xid=%llu has_cursor=%d %ux%u hot=%d,%d has_pixel=%d pixel=0x%08X\n",
                static_cast<unsigned long long>(r.xid), r.has_cursor ? 1 : 0, r.width, r.height,
                r.xhot, r.yhot, r.has_pixel ? 1 : 0, r.pixel_bgra);
    std::fflush(stdout);

    if (!r.ok || !r.has_cursor || !r.has_pixel) {
        std::printf("CURSOR-QUERY: FAIL (no cursor / no pixel)\n");
        return 1;
    }
    if (have_expect && r.pixel_bgra != expect_bgra) {
        std::printf("CURSOR-QUERY: FAIL (pixel 0x%08X != expected 0x%08X)\n", r.pixel_bgra,
                    expect_bgra);
        return 1;
    }
    std::printf("CURSOR-QUERY: PASS\n");
    return 0;
}
