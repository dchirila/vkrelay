// the dependency-free PNG encoder (linux/observe/png_write.h). Dual-platform -- the
// encoder is pure C++. Verifies the PNG signature + IHDR, that every chunk's CRC-32 is correct,
// that the stored-deflate IDAT round-trips back to the exact filtered RGBA rows (B<->R swapped from
// the BGRA source) with a correct Adler-32 trailer, the degenerate-input guards, and a multi-block
// payload (> 65535 bytes -> more than one stored block).
#include "linux/observe/png_write.h"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace vkr;

namespace {

std::uint32_t rd_u32_be(const std::string& s, std::size_t off) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3])));
}

unsigned char at(const std::string& s, std::size_t i) {
    return static_cast<unsigned char>(s[i]);
}

void test_encode_roundtrip() {
    const int W = 2, H = 3;
    std::vector<unsigned char> bgra;
    for (int i = 0; i < W * H; ++i) {
        bgra.push_back(static_cast<unsigned char>(0x10 + i)); // B
        bgra.push_back(static_cast<unsigned char>(0x20 + i)); // G
        bgra.push_back(static_cast<unsigned char>(0x30 + i)); // R
        bgra.push_back(static_cast<unsigned char>(0x40 + i)); // A
    }
    const std::string png = observe::encode_png_bgra(bgra.data(), W, H, W * 4);
    VKR_CHECK(png.size() > 8);

    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) {
        VKR_CHECK_EQ(at(png, i), sig[i]);
    }

    // Walk chunks: every CRC must verify; capture IHDR fields + the IDAT bytes.
    std::string idat;
    bool saw_ihdr = false, saw_iend = false;
    std::size_t off = 8;
    while (off + 12 <= png.size()) {
        const std::uint32_t len = rd_u32_be(png, off);
        const std::string type = png.substr(off + 4, 4);
        VKR_CHECK(off + 12 + len <= png.size());
        const std::uint32_t crc_stored = rd_u32_be(png, off + 8 + len);
        const std::uint32_t crc_calc = observe::png_detail::crc32(
            reinterpret_cast<const unsigned char*>(png.data() + off + 4), 4 + len);
        VKR_CHECK_EQ(crc_stored, crc_calc);
        if (type == "IHDR") {
            saw_ihdr = true;
            VKR_CHECK_EQ(len, static_cast<std::uint32_t>(13));
            VKR_CHECK_EQ(rd_u32_be(png, off + 8), static_cast<std::uint32_t>(W));
            VKR_CHECK_EQ(rd_u32_be(png, off + 12), static_cast<std::uint32_t>(H));
            VKR_CHECK_EQ(at(png, off + 16), static_cast<unsigned char>(8)); // bit depth
            VKR_CHECK_EQ(at(png, off + 17), static_cast<unsigned char>(6)); // RGBA
        } else if (type == "IDAT") {
            idat += png.substr(off + 8, len);
        } else if (type == "IEND") {
            saw_iend = true;
        }
        off += 12 + len;
    }
    VKR_CHECK(saw_ihdr && saw_iend);
    VKR_CHECK_EQ(off, png.size());

    // Parse the zlib stored stream: 2-byte header, stored blocks, big-endian Adler-32 trailer.
    VKR_CHECK(idat.size() >= 2 + 5 + 4);
    VKR_CHECK_EQ(at(idat, 0), static_cast<unsigned char>(0x78));
    std::string raw;
    std::size_t p = 2;
    bool final_block = false;
    while (!final_block) {
        VKR_CHECK(p + 5 <= idat.size());
        const unsigned char hdr = at(idat, p);
        VKR_CHECK_EQ(static_cast<int>(hdr & 0x06), 0); // BTYPE == 00 (stored)
        final_block = (hdr & 1) != 0;
        const std::uint16_t blen =
            static_cast<std::uint16_t>(at(idat, p + 1) | (at(idat, p + 2) << 8));
        const std::uint16_t nlen =
            static_cast<std::uint16_t>(at(idat, p + 3) | (at(idat, p + 4) << 8));
        VKR_CHECK_EQ(static_cast<std::uint16_t>(~blen), nlen);
        p += 5;
        VKR_CHECK(p + blen <= idat.size());
        raw.append(idat, p, blen);
        p += blen;
    }
    VKR_CHECK_EQ(p + 4, idat.size());
    VKR_CHECK_EQ(rd_u32_be(idat, p),
                 observe::png_detail::adler32(reinterpret_cast<const unsigned char*>(raw.data()),
                                              raw.size()));

    // De-filter (all filter 0) + compare back to the input (B<->R swapped).
    const std::size_t row = 1 + static_cast<std::size_t>(W) * 4;
    VKR_CHECK_EQ(raw.size(), row * H);
    for (int y = 0; y < H; ++y) {
        VKR_CHECK_EQ(at(raw, static_cast<std::size_t>(y) * row), static_cast<unsigned char>(0));
        for (int x = 0; x < W; ++x) {
            const std::size_t ro =
                static_cast<std::size_t>(y) * row + 1 + static_cast<std::size_t>(x) * 4;
            const std::size_t bo = (static_cast<std::size_t>(y) * W + x) * 4;
            VKR_CHECK_EQ(at(raw, ro + 0), bgra[bo + 2]); // R
            VKR_CHECK_EQ(at(raw, ro + 1), bgra[bo + 1]); // G
            VKR_CHECK_EQ(at(raw, ro + 2), bgra[bo + 0]); // B
            VKR_CHECK_EQ(at(raw, ro + 3), bgra[bo + 3]); // A
        }
    }
}

void test_encode_degenerate() {
    std::vector<unsigned char> px(16, 0);
    VKR_CHECK(observe::encode_png_bgra(nullptr, 2, 2, 8).empty());
    VKR_CHECK(observe::encode_png_bgra(px.data(), 0, 2, 8).empty());
    VKR_CHECK(observe::encode_png_bgra(px.data(), 2, 0, 8).empty());
    VKR_CHECK(observe::encode_png_bgra(px.data(), 2, 2, 4).empty()); // stride < w*4
}

// A payload > 65535 bytes spans more than one stored block; the file must still walk + CRC clean.
void test_encode_multiblock() {
    const int W = 200, H = 100; // raw = 100*(1 + 800) = 80100 bytes -> 2 stored blocks
    std::vector<unsigned char> bgra(static_cast<std::size_t>(W) * H * 4, 0x77);
    const std::string png = observe::encode_png_bgra(bgra.data(), W, H, W * 4);
    VKR_CHECK(!png.empty());
    std::size_t off = 8;
    bool ok = true;
    while (off + 12 <= png.size()) {
        const std::uint32_t len = rd_u32_be(png, off);
        if (off + 12 + len > png.size()) {
            ok = false;
            break;
        }
        const std::uint32_t crc_stored = rd_u32_be(png, off + 8 + len);
        const std::uint32_t crc_calc = observe::png_detail::crc32(
            reinterpret_cast<const unsigned char*>(png.data() + off + 4), 4 + len);
        if (crc_stored != crc_calc) {
            ok = false;
            break;
        }
        off += 12 + len;
    }
    VKR_CHECK(ok);
    VKR_CHECK_EQ(off, png.size());
}

} // namespace

int main() {
    test_encode_roundtrip();
    test_encode_degenerate();
    test_encode_multiblock();
    return vkr::test::finish("unit_png");
}
