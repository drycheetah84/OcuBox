#include "common/compress.h"
#include <stdexcept>
#include <format>

#include <lzma.h>
#include <bzlib.h>
#include <zlib.h>

namespace hw::compress {

Bytes xz_decode(std::span<const uint8_t> in, size_t expected) {
    Bytes out(expected ? expected : (in.size() * 4 + 4096));
    uint64_t memlimit = UINT64_MAX;
    size_t in_pos = 0, out_pos = 0;
    lzma_ret r = lzma_stream_buffer_decode(
        &memlimit, 0, nullptr,
        in.data(), &in_pos, in.size(),
        out.data(), &out_pos, out.size());
    if (r != LZMA_OK)
        throw std::runtime_error(std::format("xz_decode failed: lzma_ret={}", (int)r));
    out.resize(out_pos);
    return out;
}

Bytes bz2_decode(std::span<const uint8_t> in, size_t expected) {
    unsigned int dest_len = (unsigned int)(expected ? expected : in.size() * 4 + 4096);
    Bytes out(dest_len);
    int r = BZ2_bzBuffToBuffDecompress(
        reinterpret_cast<char*>(out.data()), &dest_len,
        const_cast<char*>(reinterpret_cast<const char*>(in.data())),
        (unsigned int)in.size(), 0, 0);
    if (r != BZ_OK)
        throw std::runtime_error(std::format("bz2_decode failed: bz_ret={}", r));
    out.resize(dest_len);
    return out;
}

static Bytes inflate_impl(std::span<const uint8_t> in, int window_bits) {
    z_stream zs{};
    if (inflateInit2(&zs, window_bits) != Z_OK)
        throw std::runtime_error("inflate: inflateInit2 failed");

    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = (uInt)in.size();

    Bytes out;
    out.resize(in.size() * 4 + 65536);
    int ret;
    do {
        if (zs.total_out >= out.size()) out.resize(out.size() * 2);
        zs.next_out = out.data() + zs.total_out;
        zs.avail_out = (uInt)(out.size() - zs.total_out);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            throw std::runtime_error(std::format("inflate ret={}", ret));
        }
    } while (ret != Z_STREAM_END);

    out.resize(zs.total_out);
    inflateEnd(&zs);
    return out;
}

// 15 window bits + 32 => auto-detect gzip or zlib header.
Bytes gzip_decode(std::span<const uint8_t> in) { return inflate_impl(in, 15 + 32); }

// Negative window bits => raw DEFLATE with no header/trailer.
Bytes inflate_raw(std::span<const uint8_t> in) { return inflate_impl(in, -15); }

} // namespace hw::compress
