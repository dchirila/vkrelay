// vkrelay2 popup-state query (boundary-smoke helper).
//
// A sidecar-plane RPC client (NO xcb, NO Vulkan): it proves a guest popup's representation from
// OUTSIDE the worker, the worker-visible way -- it connects to the worker's sidecar
// plane (SidecarHello + token, like the real sidecar), issues a DebugEnumWindows query, and exits 0
// iff the enumeration shows the popup XID represented with is_popup=true + owner_xid ==
// --expect-owner, and the owner XID represented as a registered NON-popup toplevel. So the boundary
// smoke asserts the worker's popup owner/z-order linkage over the wire, not by scraping a log. The
// worker serves one sidecar connection at a time, so the smoke runs this AFTER the capturing
// sidecar has been stopped (freeing the plane).
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
    std::uint64_t popup_xid = 0;
    std::uint64_t expect_owner = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "popup-query: missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--connect") {
            connect = next("--connect");
        } else if (arg == "--sidecar-token") {
            token = next("--sidecar-token");
        } else if (arg == "--popup-xid") {
            popup_xid = std::strtoull(next("--popup-xid"), nullptr, 0);
        } else if (arg == "--expect-owner") {
            expect_owner = std::strtoull(next("--expect-owner"), nullptr, 0);
        } else {
            std::fprintf(stderr, "popup-query: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (popup_xid == 0 || expect_owner == 0) {
        std::fprintf(stderr, "popup-query: --popup-xid and --expect-owner are required\n");
        return 2;
    }

    std::string host;
    int port = 0;
    if (!parse_host_port(connect, host, port)) {
        std::fprintf(stderr, "popup-query: invalid --connect %s\n", connect.c_str());
        return 2;
    }

    std::unique_ptr<vkr::transport::Connection> conn;
    try {
        conn = vkr::transport::tcp_connect(host, port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "popup-query: cannot reach sidecar plane %s: %s\n", connect.c_str(),
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
        std::fprintf(stderr, "popup-query: sidecar handshake rejected\n");
        return 3;
    }

    vkr::vkrpc::RpcChannel rpc(*conn);
    vkr::vkrpc::RpcMessage msg;
    msg.op = static_cast<std::uint32_t>(vkr::sidecar::SidecarOp::DebugEnumWindows);
    msg.request_id = 1;
    msg.body = vkr::sidecar::SidecarDebugEnumWindowsRequest{}.to_body().dump(0);
    rpc.send(msg);
    vkr::vkrpc::RpcMessage resp;
    if (!rpc.recv(resp) || resp.status != 0) {
        std::fprintf(stderr, "popup-query: DebugEnumWindows rpc failed (status %u)\n", resp.status);
        return 3;
    }
    vkr::json::Value rbody;
    std::string err;
    if (!vkr::json::Value::try_parse(resp.body, rbody, err)) {
        std::fprintf(stderr, "popup-query: bad response body: %s\n", err.c_str());
        return 3;
    }
    const vkr::sidecar::SidecarDebugEnumWindowsResponse r =
        vkr::sidecar::SidecarDebugEnumWindowsResponse::from_body(rbody);

    const vkr::sidecar::SidecarWindowInfo* popup = nullptr;
    const vkr::sidecar::SidecarWindowInfo* owner = nullptr;
    for (const auto& w : r.windows) {
        if (w.xid == popup_xid) {
            popup = &w;
        } else if (w.xid == expect_owner) {
            owner = &w;
        }
    }
    std::printf("POPUP-QUERY: windows=%zu popup_found=%d owner_found=%d\n", r.windows.size(),
                popup != nullptr ? 1 : 0, owner != nullptr ? 1 : 0);
    if (popup != nullptr) {
        std::printf("POPUP-QUERY: popup xid=%llu rep=%s is_popup=%d owner=0x%llx kind=%u\n",
                    static_cast<unsigned long long>(popup->xid), popup->representation.c_str(),
                    popup->is_popup ? 1 : 0, static_cast<unsigned long long>(popup->owner_xid),
                    popup->popup_kind);
    }
    std::fflush(stdout);

    if (popup == nullptr) {
        std::printf("POPUP-QUERY: FAIL (popup xid not represented)\n");
        return 1;
    }
    if (!popup->is_popup || popup->owner_xid != expect_owner) {
        std::printf("POPUP-QUERY: FAIL (popup not owned by 0x%llx)\n",
                    static_cast<unsigned long long>(expect_owner));
        return 1;
    }
    if (owner == nullptr || owner->is_popup || !owner->toplevel_registered) {
        std::printf("POPUP-QUERY: FAIL (owner not a registered non-popup toplevel)\n");
        return 1;
    }
    std::printf("POPUP-QUERY: PASS\n");
    return 0;
}
