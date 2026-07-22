// Deterministic decoder fuzz smoke for the Vulkan RPC envelope. New wire decoders ship with a fuzz
// target and must be total.
//
// Feeds random and adversarially-shaped bytes through the frame decoder, the RPC
// header decoder, and the JSON-bodied capability decoders. None may crash, read
// out of bounds, or let an exception escape.
#include "common/protocol/wire.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"
#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <string>

namespace {

std::uint64_t g_state = 0x243F6A8885A308D3ull;
std::uint8_t next_byte() {
    g_state = g_state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint8_t>(g_state >> 33);
}

// Runs every RPC-layer decoder over `buf`. Any escaping exception is a failure.
void exercise(const std::string& buf) {
    try {
        // 1) decode_rpc directly on arbitrary bytes.
        vkr::vkrpc::RpcMessage msg;
        std::string err;
        if (vkr::vkrpc::decode_rpc(buf, msg, err)) {
            // The body is uninterpreted bytes; try to parse it as a capability
            // request/response to exercise those decoders too.
            vkr::json::Value body;
            std::string jerr;
            if (vkr::json::Value::try_parse(msg.body, body, jerr)) {
                (void) vkr::vkrpc::CapabilitiesRequest::from_body(body);
                (void) vkr::vkrpc::CapabilitiesResponse::from_body(body);
                (void) vkr::vkrpc::CreateInstanceRequest::from_body(body);
                (void) vkr::vkrpc::CreateInstanceResponse::from_body(body);
                (void) vkr::vkrpc::EnumeratePhysicalDevicesRequest::from_body(body);
                (void) vkr::vkrpc::EnumeratePhysicalDevicesResponse::from_body(body);
                (void) vkr::vkrpc::CreateDeviceRequest::from_body(body);
                (void) vkr::vkrpc::CreateDeviceResponse::from_body(body);
                (void) vkr::vkrpc::HandleRequest::from_body(body);
                (void) vkr::vkrpc::StatusResponse::from_body(body);
                (void) vkr::vkrpc::PhysicalDeviceEntry::from_body(body);
                (void) vkr::vkrpc::GetDeviceQueueRequest::from_body(body);
                (void) vkr::vkrpc::GetDeviceQueueResponse::from_body(body);
                (void) vkr::vkrpc::CreateCommandPoolRequest::from_body(body);
                (void) vkr::vkrpc::CreateCommandPoolResponse::from_body(body);
                (void) vkr::vkrpc::AllocateCommandBuffersRequest::from_body(body);
                (void) vkr::vkrpc::AllocateCommandBuffersResponse::from_body(body);
                (void) vkr::vkrpc::FreeCommandBuffersRequest::from_body(body);
                (void) vkr::vkrpc::CreateFenceRequest::from_body(body);
                (void) vkr::vkrpc::CreateFenceResponse::from_body(body);
                (void) vkr::vkrpc::CreateSemaphoreRequest::from_body(body);
                (void) vkr::vkrpc::CreateSemaphoreResponse::from_body(body);
                (void) vkr::vkrpc::AllocateMemoryRequest::from_body(body);
                (void) vkr::vkrpc::AllocateMemoryResponse::from_body(body);
                (void) vkr::vkrpc::CreateSurfaceRequest::from_body(body);
                (void) vkr::vkrpc::CreateSurfaceResponse::from_body(body);
                (void) vkr::vkrpc::CreateSwapchainRequest::from_body(body);
                (void) vkr::vkrpc::CreateSwapchainResponse::from_body(body);
                (void) vkr::vkrpc::GetSwapchainImagesRequest::from_body(body);
                (void) vkr::vkrpc::GetSwapchainImagesResponse::from_body(body);
                (void) vkr::vkrpc::AcquireNextImageRequest::from_body(body);
                (void) vkr::vkrpc::AcquireNextImageResponse::from_body(body);
                (void) vkr::vkrpc::QueuePresentRequest::from_body(body);
                (void) vkr::vkrpc::QueuePresentResponse::from_body(body);
                (void) vkr::vkrpc::RecordCommandBufferRequest::from_body(body);
                (void) vkr::vkrpc::QueueSubmitRequest::from_body(body);
                (void) vkr::vkrpc::QueueSubmitResponse::from_body(body);
                (void) vkr::vkrpc::ResetFencesRequest::from_body(body);
                (void) vkr::vkrpc::WaitForFencesRequest::from_body(body);
                (void) vkr::vkrpc::WaitForFencesResponse::from_body(body);
                (void) vkr::vkrpc::GetSurfaceCapabilitiesRequest::from_body(body);
                (void) vkr::vkrpc::GetSurfaceCapabilitiesResponse::from_body(body);
                (void) vkr::vkrpc::GetSurfaceFormatsRequest::from_body(body);
                (void) vkr::vkrpc::GetSurfaceFormatsResponse::from_body(body);
                (void) vkr::vkrpc::GetSurfacePresentModesRequest::from_body(body);
                (void) vkr::vkrpc::GetSurfacePresentModesResponse::from_body(body);
                (void) vkr::vkrpc::GetSurfaceSupportRequest::from_body(body);
                (void) vkr::vkrpc::GetSurfaceSupportResponse::from_body(body);
                // Draw surface.
                (void) vkr::vkrpc::CreateImageViewRequest::from_body(body);
                (void) vkr::vkrpc::CreateImageViewResponse::from_body(body);
                (void) vkr::vkrpc::CreateShaderModuleResponse::from_body(body);
                (void) vkr::vkrpc::CreateRenderPassRequest::from_body(body);
                (void) vkr::vkrpc::CreateRenderPassResponse::from_body(body);
                (void) vkr::vkrpc::CreateFramebufferRequest::from_body(body);
                (void) vkr::vkrpc::CreateFramebufferResponse::from_body(body);
                (void) vkr::vkrpc::CreatePipelineLayoutRequest::from_body(body);
                (void) vkr::vkrpc::CreatePipelineLayoutResponse::from_body(body);
                (void) vkr::vkrpc::CreateGraphicsPipelinesRequest::from_body(body);
                (void) vkr::vkrpc::CreateGraphicsPipelinesResponse::from_body(body);
                // Host-visible memory + buffers.
                (void) vkr::vkrpc::GetPhysicalDeviceMemoryPropertiesRequest::from_body(body);
                (void) vkr::vkrpc::GetPhysicalDeviceMemoryPropertiesResponse::from_body(body);
                (void) vkr::vkrpc::CreateBufferRequest::from_body(body);
                (void) vkr::vkrpc::CreateBufferResponse::from_body(body);
                (void) vkr::vkrpc::BindBufferMemoryRequest::from_body(body);
                // Descriptor surface. update_descriptor_sets is the headline target --
                // attacker-controlled write/binding counts, array elements, and wide offset/range.
                (void) vkr::vkrpc::CreateDescriptorSetLayoutRequest::from_body(body);
                (void) vkr::vkrpc::CreateDescriptorSetLayoutResponse::from_body(body);
                (void) vkr::vkrpc::CreateDescriptorPoolRequest::from_body(body);
                (void) vkr::vkrpc::CreateDescriptorPoolResponse::from_body(body);
                (void) vkr::vkrpc::AllocateDescriptorSetsRequest::from_body(body);
                (void) vkr::vkrpc::AllocateDescriptorSetsResponse::from_body(body);
                (void) vkr::vkrpc::UpdateDescriptorSetsRequest::from_body(body);
                // Textures + depth: format props + image create/bind decoders.
                (void) vkr::vkrpc::GetPhysicalDeviceFormatPropertiesRequest::from_body(body);
                (void) vkr::vkrpc::GetPhysicalDeviceFormatPropertiesResponse::from_body(body);
                (void) vkr::vkrpc::CreateImageRequest::from_body(body);
                (void) vkr::vkrpc::CreateImageResponse::from_body(body);
                (void) vkr::vkrpc::BindImageMemoryRequest::from_body(body);
                // Sampler: create-sampler decoders. (The
                // combined-image-sampler image-info write + the copy/barrier recorded commands ride
                // UpdateDescriptorSetsRequest / RecordCommandBufferRequest, already fuzzed above.)
                (void) vkr::vkrpc::CreateSamplerRequest::from_body(body);
                (void) vkr::vkrpc::CreateSamplerResponse::from_body(body);
                // Sidecar plane: the JSON-bodied lifecycle + chrome decoders.
                (void) vkr::sidecar::SidecarRegisterToplevelRequest::from_body(body);
                (void) vkr::sidecar::SidecarUpdateToplevelRequest::from_body(body);
                (void) vkr::sidecar::SidecarUnregisterToplevelRequest::from_body(body);
                (void) vkr::sidecar::SidecarToplevelResponse::from_body(body);
                (void) vkr::sidecar::SidecarPaintResponse::from_body(body);
                (void) vkr::sidecar::SidecarDebugChromeStateRequest::from_body(body);
                (void) vkr::sidecar::SidecarDebugChromeStateResponse::from_body(body);
            }
            // The binary-tail decoders run on the RAW body (not JSON): they must be total over
            // arbitrary bytes -- no OOB read on a hostile [u32 json_len][...] prefix. The chrome
            // paint decoder is the headline target here: attacker-controlled dimensions / stride /
            // payload_size with wide-arithmetic dirty-rect bounds + an exact tail-length check.
            std::string smerr;
            (void) vkr::vkrpc::CreateShaderModuleRequest::from_wire(msg.body, smerr);
            std::string wmerr;
            (void) vkr::vkrpc::WriteMemoryRangesRequest::from_wire(msg.body, wmerr);
            std::string pcerr;
            (void) vkr::sidecar::SidecarPaintChromeRequest::from_wire(msg.body, pcerr);
            // The capture RESPONSE decoder runs on the
            // QUERY-TOOL side, so fuzz the producer's [u32 json_len][hdr-json][BGRA tail] -- caps,
            // dims, stride, exact tail, and the non-ok-carries-no-tail rule must all stay total on
            // hostile bytes.
            std::string cwerr;
            (void) vkr::sidecar::SidecarDebugCaptureWindowResponse::from_wire(msg.body, cwerr);
        }
        std::string pipeline_err;
        (void) vkr::vkrpc::CreateGraphicsPipelinesRequest::from_wire(buf, pipeline_err);
        pipeline_err.clear();
        (void) vkr::vkrpc::CreateComputePipelinesRequest::from_wire(buf, pipeline_err);

        // 2) Framed path: decode a frame, then the RPC header from its payload.
        std::size_t consumed = 0;
        std::string payload;
        std::string ferr;
        if (vkr::protocol::try_decode_frame(buf, consumed, payload, ferr) ==
            vkr::protocol::FrameStatus::Ok) {
            vkr::vkrpc::RpcMessage framed;
            std::string rerr;
            (void) vkr::vkrpc::decode_rpc(payload, framed, rerr);
        }
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("rpc decoder threw: ") + e.what());
    } catch (...) {
        ::vkr::test::report_fail(__FILE__, __LINE__, "rpc decoder threw non-std exception");
    }
}

} // namespace

int main() {
    // Pure random buffers of varied length (straddling the 12-byte header).
    for (int iter = 0; iter < 20000; ++iter) {
        const std::size_t len = next_byte() % 64;
        std::string buf;
        buf.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            buf.push_back(static_cast<char>(next_byte()));
        }
        exercise(buf);
    }

    // Frames with attacker-controlled length headers + random bodies.
    for (int iter = 0; iter < 20000; ++iter) {
        std::string buf(4, '\0');
        const std::uint32_t claimed = static_cast<std::uint32_t>(next_byte()) |
                                      (static_cast<std::uint32_t>(next_byte()) << 8) |
                                      (static_cast<std::uint32_t>(next_byte()) << 16) |
                                      (static_cast<std::uint32_t>(next_byte()) << 24);
        vkr::protocol::store_le32(claimed, reinterpret_cast<unsigned char*>(&buf[0]));
        const std::size_t body = next_byte();
        for (std::size_t i = 0; i < body; ++i) {
            buf.push_back(static_cast<char>(next_byte()));
        }
        exercise(buf);
    }

    // Valid RPC frames wrapping JSON-ish bodies, to stress the body decoders.
    const char* fragments[] = {
        "{}",
        "{\"requested_api_major\":1,\"requested_api_minor\":3}",
        "{\"requested_api_major\":\"x\"}",
        "{\"ok\":true,\"device\":{\"vendor_id\":99}}",
        "{\"device\":[]}",
        "not json",
        "",
    };
    for (const char* fragment : fragments) {
        vkr::vkrpc::RpcMessage m;
        m.op = static_cast<std::uint32_t>(vkr::vkrpc::RpcOp::NegotiateCapabilities);
        m.request_id = 1;
        m.body = fragment;
        const std::string payload = vkr::vkrpc::encode_rpc(m);
        exercise(payload);
        exercise(vkr::protocol::encode_frame(payload));
    }

    return vkr::test::finish("fuzz_vkrpc");
}
