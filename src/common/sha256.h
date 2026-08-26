// Minimal standalone SHA-256 (FIPS 180-4). Used to build liblp super-partition
// metadata checksums; not performance-critical.
#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace hw::crypto {

// Compute the SHA-256 digest of `len` bytes at `data` into `out` (32 bytes).
void sha256(const void* data, size_t len, uint8_t out[32]);

} // namespace hw::crypto
