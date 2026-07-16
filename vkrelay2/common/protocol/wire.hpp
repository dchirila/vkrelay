// Length-prefixed frame envelope for the vkrelay2 control plane
// used by both control and Vulkan RPC messages.
//
// Wire layout: [u32 little-endian payload length][payload bytes]. The payload
// is a JSON control message (see messages.hpp). Bulk Vulkan object transfers
// will use a separate chunked path, so this control-plane cap is
// deliberately small to bound denial-of-service from a misbehaving peer.
//
// try_decode_frame() is total: it never throws and never reads out of bounds on
// arbitrary input, which is the property the fuzz smoke test pins down.
#ifndef VKRELAY2_COMMON_PROTOCOL_WIRE_HPP
#define VKRELAY2_COMMON_PROTOCOL_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace vkr::protocol {

constexpr std::size_t kMaxFrameBytes = 16u * 1024u * 1024u; // control-plane bound

struct FrameError : std::runtime_error {
    explicit FrameError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class FrameStatus {
    Ok,       // a complete frame was decoded
    NeedMore, // buffer holds a partial frame; read more bytes
    Error     // malformed or oversized frame; the stream is unusable
};

std::uint32_t load_le32(const unsigned char* p) noexcept;
void store_le32(std::uint32_t value, unsigned char* p) noexcept;

// Prepends the 4-byte length header to `payload`. Throws FrameError if payload
// exceeds kMaxFrameBytes.
std::string encode_frame(const std::string& payload);

// Attempts to decode one frame from the front of `buffer`. On Ok, `payload`
// receives the frame body and `consumed` the number of leading bytes to drop.
// On NeedMore/Error, neither output is meaningful (Error sets `error`).
FrameStatus try_decode_frame(const std::string& buffer, std::size_t& consumed, std::string& payload,
                             std::string& error);

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_WIRE_HPP
