// Minimal, dependency-free PNG encoder for vkrelay2 capture output.
//
// Encodes a top-down BGRA8 source buffer as an 8-bit RGBA PNG using STORED (uncompressed) DEFLATE
// blocks, so there is no libpng / zlib / stb dependency anywhere in the tree. The artifacts this
// serves are small (placeholder chrome + cursor images), so the size cost of stored deflate is
// irrelevant; correctness + zero-deps is the goal. Pure standard C++ -> compiles + is unit-tested
// on BOTH toolchains (the encoder is dual-platform; only the capture tool that uses it is
// Linux-only).
//
// PNG layout: 8-byte signature, IHDR (RGBA8, no interlace), one IDAT (a zlib stream of stored
// blocks over the filter-0 rows), IEND. CRC-32 per chunk; Adler-32 over the uncompressed data.
#ifndef VKRELAY2_LINUX_OBSERVE_PNG_WRITE_H
#define VKRELAY2_LINUX_OBSERVE_PNG_WRITE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkr::observe {
namespace png_detail {

// PNG CRC-32 (polynomial 0xEDB88320), table-less so the header has no static state.
inline std::uint32_t crc32(const unsigned char* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Adler-32 over the uncompressed data (zlib trailer), processed in NMAX=5552 runs so the sums never
// overflow before the modulo.
inline std::uint32_t adler32(const unsigned char* data, std::size_t len) {
    std::uint32_t a = 1, b = 0;
    std::size_t i = 0;
    while (i < len) {
        const std::size_t n = (len - i < 5552) ? (len - i) : 5552;
        for (std::size_t j = 0; j < n; ++j) {
            a += data[i + j];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        i += n;
    }
    return (b << 16) | a;
}

inline void put_u32_be(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

// Append a PNG chunk: length, type, data, CRC(type+data).
inline void put_chunk(std::string& out, const char type[4], const std::string& data) {
    put_u32_be(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t crc_start = out.size();
    out.append(type, 4);
    out += data;
    const std::uint32_t crc =
        crc32(reinterpret_cast<const unsigned char*>(out.data() + crc_start), 4 + data.size());
    put_u32_be(out, crc);
}

} // namespace png_detail

// Encode a top-down BGRA8 image (`stride` bytes per row, must be >= width*4) as an 8-bit RGBA PNG.
// Returns the PNG file bytes, or "" on a degenerate input (null / non-positive dims / short
// stride).
inline std::string encode_png_bgra(const unsigned char* bgra, int width, int height, int stride) {
    if (bgra == nullptr || width <= 0 || height <= 0 || stride < width * 4) {
        return std::string();
    }
    // The uncompressed zlib payload: each row is a filter byte (0 = None) + RGBA pixels, swapping
    // B<->R from the BGRA source (alpha kept).
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + static_cast<std::size_t>(width) * 4));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: None
        const unsigned char* src =
            bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; ++x) {
            const unsigned char* p = src + static_cast<std::size_t>(x) * 4;
            raw.push_back(p[2]); // R
            raw.push_back(p[1]); // G
            raw.push_back(p[0]); // B
            raw.push_back(p[3]); // A
        }
    }
    // zlib stream: 2-byte header (0x78 0x01: deflate/32K window, (0x7801 % 31 == 0)) + stored
    // DEFLATE blocks (each <= 65535 bytes) + big-endian Adler-32 of `raw`.
    std::string zlib;
    zlib.push_back(static_cast<char>(0x78));
    zlib.push_back(static_cast<char>(0x01));
    std::size_t off = 0;
    do {
        const std::size_t block = (raw.size() - off < 65535) ? (raw.size() - off) : 65535;
        const bool final_block = (off + block >= raw.size());
        zlib.push_back(static_cast<char>(final_block ? 1 : 0)); // BFINAL + BTYPE=00 (stored)
        const std::uint16_t len = static_cast<std::uint16_t>(block);
        const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
        zlib.push_back(static_cast<char>(len & 0xFF));
        zlib.push_back(static_cast<char>((len >> 8) & 0xFF));
        zlib.push_back(static_cast<char>(nlen & 0xFF));
        zlib.push_back(static_cast<char>((nlen >> 8) & 0xFF));
        zlib.append(reinterpret_cast<const char*>(raw.data() + off), block);
        off += block;
    } while (off < raw.size());
    png_detail::put_u32_be(zlib, png_detail::adler32(raw.data(), raw.size()));

    std::string out;
    const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    out.append(reinterpret_cast<const char*>(sig), 8);
    std::string ihdr;
    png_detail::put_u32_be(ihdr, static_cast<std::uint32_t>(width));
    png_detail::put_u32_be(ihdr, static_cast<std::uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // color type: RGBA
    ihdr.push_back(0); // compression: deflate
    ihdr.push_back(0); // filter: standard
    ihdr.push_back(0); // interlace: none
    png_detail::put_chunk(out, "IHDR", ihdr);
    png_detail::put_chunk(out, "IDAT", zlib);
    png_detail::put_chunk(out, "IEND", std::string());
    return out;
}

} // namespace vkr::observe

#endif // VKRELAY2_LINUX_OBSERVE_PNG_WRITE_H
