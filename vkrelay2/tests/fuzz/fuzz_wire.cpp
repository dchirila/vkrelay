// Deterministic decoder fuzz smoke test: malformed messages must not crash the supervisor.
//
// Feeds large volumes of random and adversarially-shaped bytes through
// try_decode_frame() and decode_message(). The decoders must be total: never
// crash, never read out of bounds, never let an exception escape. A real
// libFuzzer target can wrap the same `exercise()` later; this version is a
// dependency-free, deterministic CTest.
#include "common/protocol/messages.hpp"
#include "common/protocol/wire.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <string>

namespace {

std::uint64_t g_state = 0x9E3779B97F4A7C15ull;
std::uint8_t next_byte() {
    g_state = g_state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint8_t>(g_state >> 33);
}

// Runs both decoders over `buf`. Any escaping exception is a failure.
void exercise(const std::string& buf) {
    try {
        std::size_t consumed = 0;
        std::string payload;
        std::string err;
        const vkr::protocol::FrameStatus status =
            vkr::protocol::try_decode_frame(buf, consumed, payload, err);
        if (status == vkr::protocol::FrameStatus::Ok) {
            vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
            vkr::json::Value body;
            std::string msg_err;
            vkr::protocol::decode_message(payload, type, body, msg_err);
        }
        // The payload bytes themselves are also fed to the message decoder, so a
        // garbage frame body is exercised even on NeedMore/Error inputs.
        vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
        vkr::json::Value body;
        std::string msg_err;
        vkr::protocol::decode_message(buf, type, body, msg_err);
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("decoder threw: ") + e.what());
    } catch (...) {
        ::vkr::test::report_fail(__FILE__, __LINE__, "decoder threw non-std exception");
    }
}

} // namespace

int main() {
    // Pure random buffers of varied length.
    for (int iter = 0; iter < 20000; ++iter) {
        const std::size_t len = next_byte() % 300;
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

    // Valid frames wrapping JSON-ish payloads, to stress decode_message.
    const char* fragments[] = {
        "{",
        "{}",
        "{\"type\":}",
        "{\"type\":\"hello\"}",
        "{\"type\":\"hello\",\"body\":{\"protocol_version\":\"x\"}}",
        "{\"type\":123}",
        "[\"hello\"]",
        "{\"type\":\"\\uZZZZ\"}",
        "null",
        "\"\\\"",
    };
    for (const char* fragment : fragments) {
        exercise(vkr::protocol::encode_frame(fragment));
        exercise(fragment);
    }

    return vkr::test::finish("fuzz_wire");
}
