// One-shot buffer decompression helpers (XZ, bzip2, gzip/zlib).
#pragma once
#include "common/bytes.h"
#include <span>

namespace hw::compress {

// Decompress a complete .xz stream. `expected` is the known output size
// (from update_engine dst extents); the output is resized to the actual size.
Bytes xz_decode(std::span<const uint8_t> in, size_t expected);

// Decompress a complete bzip2 stream.
Bytes bz2_decode(std::span<const uint8_t> in, size_t expected);

// Decompress gzip OR raw zlib (auto-detected). Used for Android ramdisks.
Bytes gzip_decode(std::span<const uint8_t> in);

// Decompress a raw DEFLATE stream (no header) -- the ZIP compression method 8.
Bytes inflate_raw(std::span<const uint8_t> in);

} // namespace hw::compress
