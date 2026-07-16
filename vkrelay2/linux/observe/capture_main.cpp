// vkrelay2 source-layer capture tool.
//
// A sidecar-plane RPC client (NO xcb, NO Vulkan): it captures a guest toplevel's FULL source-layer
// BGRA from OUTSIDE the worker -- the worker-visible, occlusion/minimize-proof way -- and writes a
// PNG + a JSON metadata sidecar. It connects to the worker's sidecar plane (SidecarHello + token,
// like the real sidecar), issues DebugCaptureWindow for an XID + layer (chrome|cursor), decodes the
// binary response, and encodes the pixels via the dependency-free png_write.h.
//
// Modes:
//   --xid <id> --layer <chrome|cursor> --out <prefix>   capture one window -> <prefix>.png/.json
//   --all --out <prefix>                                 capture EVERY enumerated window (the
//                                                        multi-window-app case): one PNG+JSON each,
//                                                        named <prefix>.0x<xid>.<layer>.png/.json
// Optional lifecycle selectors (--expected-epoch / --expected-generation / --min-last-seq) pin the
// capture to a specific lifecycle. Optional assertions (--min-unique-colors / --expect-bgra) make
// the boundary smoke a hard gate: exit 1 if the capture is degenerate or the known color is absent.
//
// Exit codes: 0 success, 1 assertion failed, 2 bad arguments, 3 connection / RPC error.
//
// The worker serves one sidecar connection at a time, so the smoke runs this AFTER the capturing
// sidecar has been stopped (freeing the plane); the placeholder/DIB + cursor survive the
// disconnect.
#include "common/protocol/messages.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"
#include "linux/observe/png_write.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

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

std::uint32_t packed_bgra(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::size_t count_unique_colors(const std::string& pixels) {
    std::set<std::uint32_t> colors;
    for (std::size_t i = 0; i + 4 <= pixels.size(); i += 4) {
        colors.insert(packed_bgra(reinterpret_cast<const unsigned char*>(pixels.data() + i)));
    }
    return colors.size();
}

bool color_present(const std::string& pixels, std::uint32_t want) {
    for (std::size_t i = 0; i + 4 <= pixels.size(); i += 4) {
        if (packed_bgra(reinterpret_cast<const unsigned char*>(pixels.data() + i)) == want) {
            return true;
        }
    }
    return false;
}

bool write_file(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

bool do_capture(vkr::vkrpc::RpcChannel& rpc, std::uint32_t request_id,
                const vkr::sidecar::SidecarDebugCaptureWindowRequest& req,
                vkr::sidecar::SidecarDebugCaptureWindowResponse& out) {
    vkr::vkrpc::RpcMessage msg;
    msg.op = static_cast<std::uint32_t>(vkr::sidecar::SidecarOp::DebugCaptureWindow);
    msg.request_id = request_id;
    msg.body = req.to_body().dump(0);
    rpc.send(msg);
    vkr::vkrpc::RpcMessage resp;
    if (!rpc.recv(resp) || resp.status != 0) {
        std::fprintf(stderr, "capture: DebugCaptureWindow rpc failed (status %u)\n", resp.status);
        return false;
    }
    std::string err;
    out = vkr::sidecar::SidecarDebugCaptureWindowResponse::from_wire(resp.body, err);
    if (!err.empty()) {
        std::fprintf(stderr, "capture: malformed response: %s\n", err.c_str());
        return false;
    }
    return true;
}

// Write the PNG (only on status "ok") + the JSON sidecar. Sets `unique_out` (unique color count, 0
// if no pixels) and `wrote_png` (whether a PNG file was written). Returns true iff ALL required
// artifacts were persisted: the PNG when the capture is "ok" (an ok capture that cannot be written
// is a tool failure) AND the JSON sidecar. For a non-ok status no PNG is
// required, so success hinges on the JSON write alone.
bool emit_artifact(const vkr::sidecar::SidecarDebugCaptureWindowResponse& r,
                   const std::string& png_path, const std::string& json_path,
                   const std::string& captured_at, std::size_t& unique_out, bool& wrote_png) {
    wrote_png = false;
    unique_out = 0;
    bool png_ok = true; // vacuously satisfied when there are no pixels to write
    if (r.ok && !r.pixels.empty()) {
        unique_out = count_unique_colors(r.pixels);
        const std::string png = vkr::observe::encode_png_bgra(
            reinterpret_cast<const unsigned char*>(r.pixels.data()), static_cast<int>(r.width),
            static_cast<int>(r.height), static_cast<int>(r.stride));
        if (!png.empty() && write_file(png_path, png)) {
            wrote_png = true;
        } else {
            std::fprintf(stderr, "capture: failed to encode/write %s\n", png_path.c_str());
            png_ok = false;
        }
    }
    vkr::json::Value m = vkr::json::Value::make_object();
    m.set("xid", vkr::json::Value(std::to_string(r.xid)));
    m.set("layer", vkr::json::Value(r.layer));
    m.set("status", vkr::json::Value(r.status));
    m.set("ok", vkr::json::Value(r.ok));
    m.set("representation", vkr::json::Value(r.representation));
    m.set("width", vkr::json::Value(static_cast<int>(r.width)));
    m.set("height", vkr::json::Value(static_cast<int>(r.height)));
    m.set("stride", vkr::json::Value(static_cast<int>(r.stride)));
    m.set("format", vkr::json::Value(r.format));
    m.set("shown", vkr::json::Value(r.shown));
    if (r.layer == vkr::sidecar::kCaptureLayerCursor) {
        m.set("xhot", vkr::json::Value(r.xhot));
        m.set("yhot", vkr::json::Value(r.yhot));
    }
    m.set("generation", vkr::json::Value(std::to_string(r.generation)));
    m.set("epoch", vkr::json::Value(std::to_string(r.epoch)));
    // Provenance hint -- the toplevel's CURRENT paint seq, NOT an exact per-pixel capture
    // sequence; a source-buffer seq alongside the pixels is a deferred follow-up.
    m.set("last_paint_seq", vkr::json::Value(std::to_string(r.last_paint_seq)));
    if (r.status == "too_large") {
        m.set("needed_bytes", vkr::json::Value(std::to_string(r.needed_bytes)));
    }
    m.set("unique_colors", vkr::json::Value(static_cast<int>(unique_out)));
    m.set("png", vkr::json::Value(wrote_png ? png_path : std::string()));
    if (!captured_at.empty()) {
        m.set("captured_at", vkr::json::Value(captured_at));
    }
    const bool json_ok = write_file(json_path, m.dump(2));
    if (!json_ok) {
        std::fprintf(stderr, "capture: failed to write %s\n", json_path.c_str());
    }
    return png_ok && json_ok;
}

std::string xid_hex(std::uint64_t xid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(xid));
    return std::string(buf);
}

} // namespace

int main(int argc, char** argv) {
    std::string connect;
    std::string token;
    std::string layer = vkr::sidecar::kCaptureLayerChrome;
    std::string out;
    std::string captured_at;
    std::uint64_t xid = 0;
    bool all = false;
    std::uint64_t expected_epoch = 0;
    std::uint64_t expected_generation = 0;
    std::uint64_t min_last_seq = 0;
    long min_unique_colors = -1;
    std::uint32_t expect_bgra = 0;
    bool have_expect = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "capture: missing value for %s\n", what);
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
        } else if (arg == "--layer") {
            layer = next("--layer");
        } else if (arg == "--out") {
            out = next("--out");
        } else if (arg == "--all") {
            all = true;
        } else if (arg == "--captured-at") {
            captured_at = next("--captured-at");
        } else if (arg == "--expected-epoch") {
            expected_epoch = std::strtoull(next("--expected-epoch"), nullptr, 0);
        } else if (arg == "--expected-generation") {
            expected_generation = std::strtoull(next("--expected-generation"), nullptr, 0);
        } else if (arg == "--min-last-seq") {
            min_last_seq = std::strtoull(next("--min-last-seq"), nullptr, 0);
        } else if (arg == "--min-unique-colors") {
            min_unique_colors = std::atol(next("--min-unique-colors"));
        } else if (arg == "--expect-bgra") {
            expect_bgra =
                static_cast<std::uint32_t>(std::strtoull(next("--expect-bgra"), nullptr, 0));
            have_expect = true;
        } else {
            std::fprintf(stderr, "capture: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (out.empty()) {
        std::fprintf(stderr, "capture: --out <prefix> is required\n");
        return 2;
    }
    // Validate --layer in BOTH modes: --all captures this layer for every
    // enumerated window, so a bad token must be rejected there too.
    if (layer != vkr::sidecar::kCaptureLayerChrome && layer != vkr::sidecar::kCaptureLayerCursor) {
        std::fprintf(stderr, "capture: --layer must be chrome or cursor\n");
        return 2;
    }
    // Single-window mode needs an explicit target: without --all, a
    // missing --xid would silently capture XID 0 and emit an `absent` sidecar.
    if (!all && xid == 0) {
        std::fprintf(stderr, "capture: --xid <id> is required (or use --all)\n");
        return 2;
    }

    std::string host;
    int port = 0;
    if (!parse_host_port(connect, host, port)) {
        std::fprintf(stderr, "capture: invalid --connect %s\n", connect.c_str());
        return 2;
    }

    std::unique_ptr<vkr::transport::Connection> conn;
    try {
        conn = vkr::transport::tcp_connect(host, port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "capture: cannot reach sidecar plane %s: %s\n", connect.c_str(),
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
        std::fprintf(stderr, "capture: sidecar handshake rejected\n");
        return 3;
    }
    vkr::vkrpc::RpcChannel rpc(*conn);
    std::uint32_t request_id = 1;

    if (all) {
        // Enumerate every window, then capture each (the multi-window-app case). One PNG+JSON each.
        vkr::vkrpc::RpcMessage eq;
        eq.op = static_cast<std::uint32_t>(vkr::sidecar::SidecarOp::DebugEnumWindows);
        eq.request_id = request_id++;
        eq.body = vkr::sidecar::SidecarDebugEnumWindowsRequest{}.to_body().dump(0);
        rpc.send(eq);
        vkr::vkrpc::RpcMessage eqr;
        vkr::json::Value eqbody;
        std::string eerr;
        if (!rpc.recv(eqr) || eqr.status != 0 ||
            !vkr::json::Value::try_parse(eqr.body, eqbody, eerr)) {
            std::fprintf(stderr, "capture: DebugEnumWindows failed (status %u)\n", eqr.status);
            return 3;
        }
        const vkr::sidecar::SidecarDebugEnumWindowsResponse en =
            vkr::sidecar::SidecarDebugEnumWindowsResponse::from_body(eqbody);
        int written = 0;
        int failed = 0;
        for (const auto& w : en.windows) {
            vkr::sidecar::SidecarDebugCaptureWindowRequest req;
            req.xid = w.xid;
            req.layer = layer;
            // Pin each capture to the lifecycle that was ENUMERATED: a
            // window can churn while the artifact sweep is in progress, so the capture's post-copy
            // re-check must reject a different lifecycle rather than silently shoot the new one.
            req.expected_epoch = w.epoch;
            req.expected_lifecycle_generation = w.generation;
            req.min_last_seq = w.last_paint_seq;
            vkr::sidecar::SidecarDebugCaptureWindowResponse r;
            if (!do_capture(rpc, request_id++, req, r)) {
                return 3;
            }
            const std::string base = out + "." + xid_hex(w.xid) + "." + layer;
            std::size_t unique = 0;
            bool wrote = false;
            const bool artifact_ok =
                emit_artifact(r, base + ".png", base + ".json", captured_at, unique, wrote);
            std::printf("CAPTURE: xid=%s layer=%s status=%s %ux%u unique=%zu png=%d\n",
                        xid_hex(w.xid).c_str(), layer.c_str(), r.status.c_str(), r.width, r.height,
                        unique, wrote ? 1 : 0);
            if (wrote) {
                ++written;
            }
            // An ok-but-unwritten capture (or a failed JSON write) is a real failure for the
            // sweep; a legitimate empty/absent layer is not.
            if (!artifact_ok) {
                ++failed;
            }
        }
        std::printf("CAPTURE: --all wrote %d of %zu windows (%d write failures)\n", written,
                    static_cast<std::size_t>(en.windows.size()), failed);
        std::fflush(stdout);
        return failed > 0 ? 1 : 0;
    }

    // Single capture.
    vkr::sidecar::SidecarDebugCaptureWindowRequest req;
    req.xid = xid;
    req.layer = layer;
    req.expected_epoch = expected_epoch;
    req.expected_lifecycle_generation = expected_generation;
    req.min_last_seq = min_last_seq;
    vkr::sidecar::SidecarDebugCaptureWindowResponse r;
    if (!do_capture(rpc, request_id++, req, r)) {
        return 3;
    }
    std::size_t unique = 0;
    bool wrote = false;
    const bool artifact_ok =
        emit_artifact(r, out + ".png", out + ".json", captured_at, unique, wrote);
    std::printf("CAPTURE: xid=%s layer=%s status=%s %ux%u unique=%zu png=%d\n",
                xid_hex(xid).c_str(), layer.c_str(), r.status.c_str(), r.width, r.height, unique,
                wrote ? 1 : 0);
    std::fflush(stdout);

    // An ok capture that could not be persisted (or a failed JSON write) is a failure even WITHOUT
    // an assertion flag -- a screenshot tool that silently drops the artifact is useless.
    if (!artifact_ok) {
        std::printf("CAPTURE: FAIL (status %s; artifact not written)\n", r.status.c_str());
        return 1;
    }

    // Assertions (the smoke's hard gate). When present, a non-ok capture is a failure.
    const bool asserting = (min_unique_colors >= 0) || have_expect;
    if (asserting && (!r.ok || !wrote)) {
        std::printf("CAPTURE: FAIL (status %s, no PNG written)\n", r.status.c_str());
        return 1;
    }
    if (min_unique_colors >= 0 && static_cast<long>(unique) < min_unique_colors) {
        std::printf("CAPTURE: FAIL (unique colors %zu < %ld)\n", unique, min_unique_colors);
        return 1;
    }
    if (have_expect && !color_present(r.pixels, expect_bgra)) {
        std::printf("CAPTURE: FAIL (expected color 0x%08X absent)\n", expect_bgra);
        return 1;
    }
    if (asserting) {
        std::printf("CAPTURE: PASS\n");
    }
    return 0;
}
