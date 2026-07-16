#include "common/protocol/wire.hpp"
#include "tests/test_assert.hpp"

#include <string>

using namespace vkr::protocol;

namespace {

void test_round_trip() {
    const std::string payload = "hello world";
    const std::string frame = encode_frame(payload);
    VKR_CHECK_EQ(frame.size(), payload.size() + 4);

    std::size_t consumed = 0;
    std::string out;
    std::string err;
    VKR_CHECK(try_decode_frame(frame, consumed, out, err) == FrameStatus::Ok);
    VKR_CHECK_EQ(out, payload);
    VKR_CHECK_EQ(consumed, frame.size());
}

void test_empty_payload() {
    const std::string frame = encode_frame("");
    VKR_CHECK_EQ(frame.size(), static_cast<std::size_t>(4));
    std::size_t consumed = 0;
    std::string out = "x";
    std::string err;
    VKR_CHECK(try_decode_frame(frame, consumed, out, err) == FrameStatus::Ok);
    VKR_CHECK(out.empty());
    VKR_CHECK_EQ(consumed, static_cast<std::size_t>(4));
}

void test_need_more() {
    std::size_t consumed = 0;
    std::string out;
    std::string err;
    // Fewer than 4 header bytes.
    VKR_CHECK(try_decode_frame(std::string("\x05", 1), consumed, out, err) ==
              FrameStatus::NeedMore);
    // Header says 10 bytes but only 3 present.
    std::string partial = encode_frame("0123456789").substr(0, 7);
    VKR_CHECK(try_decode_frame(partial, consumed, out, err) == FrameStatus::NeedMore);
}

void test_two_frames() {
    std::string buf = encode_frame("aaa") + encode_frame("bbbb");
    std::size_t consumed = 0;
    std::string out;
    std::string err;
    VKR_CHECK(try_decode_frame(buf, consumed, out, err) == FrameStatus::Ok);
    VKR_CHECK_EQ(out, std::string("aaa"));
    buf.erase(0, consumed);
    VKR_CHECK(try_decode_frame(buf, consumed, out, err) == FrameStatus::Ok);
    VKR_CHECK_EQ(out, std::string("bbbb"));
    buf.erase(0, consumed);
    VKR_CHECK(buf.empty());
}

void test_oversize_is_error() {
    std::string header(4, '\0');
    store_le32(static_cast<std::uint32_t>(kMaxFrameBytes + 1),
               reinterpret_cast<unsigned char*>(&header[0]));
    std::size_t consumed = 0;
    std::string out;
    std::string err;
    VKR_CHECK(try_decode_frame(header, consumed, out, err) == FrameStatus::Error);
    VKR_CHECK(!err.empty());
}

void test_le_round_trip() {
    unsigned char buf[4];
    store_le32(0x12345678u, buf);
    VKR_CHECK_EQ(load_le32(buf), 0x12345678u);
    VKR_CHECK_EQ(buf[0], static_cast<unsigned char>(0x78));
    VKR_CHECK_EQ(buf[3], static_cast<unsigned char>(0x12));
}

} // namespace

int main() {
    test_round_trip();
    test_empty_payload();
    test_need_more();
    test_two_frames();
    test_oversize_is_error();
    test_le_round_trip();
    return vkr::test::finish("unit_wire");
}
