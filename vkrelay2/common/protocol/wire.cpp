#include "common/protocol/wire.hpp"

namespace vkr::protocol {

std::uint32_t load_le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void store_le32(std::uint32_t value, unsigned char* p) noexcept {
    p[0] = static_cast<unsigned char>(value & 0xFF);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
}

std::string encode_frame(const std::string& payload) {
    if (payload.size() > kMaxFrameBytes) {
        throw FrameError("frame payload exceeds kMaxFrameBytes");
    }
    std::string out;
    out.resize(4);
    store_le32(static_cast<std::uint32_t>(payload.size()),
               reinterpret_cast<unsigned char*>(&out[0]));
    out += payload;
    return out;
}

FrameStatus try_decode_frame(const std::string& buffer, std::size_t& consumed, std::string& payload,
                             std::string& error) {
    if (buffer.size() < 4) {
        return FrameStatus::NeedMore;
    }
    const std::uint32_t len = load_le32(reinterpret_cast<const unsigned char*>(buffer.data()));
    if (len > kMaxFrameBytes) {
        error = "frame length " + std::to_string(len) + " exceeds kMaxFrameBytes";
        return FrameStatus::Error;
    }
    if (buffer.size() - 4 < len) {
        return FrameStatus::NeedMore;
    }
    payload.assign(buffer, 4, len);
    consumed = 4 + static_cast<std::size_t>(len);
    return FrameStatus::Ok;
}

} // namespace vkr::protocol
