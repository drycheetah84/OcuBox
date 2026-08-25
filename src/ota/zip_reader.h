// Minimal ZIP reader: locates entries and their raw data offsets so that a
// STORED entry (like an OTA's payload.bin) can be read in place without
// extracting the whole ~1GB archive.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hw::ota {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;       // 0 = stored, 8 = deflate
    uint64_t comp_size = 0;
    uint64_t uncomp_size = 0;
    uint64_t local_header_off = 0;
    uint64_t data_offset = 0;  // absolute file offset of the entry's data bytes
};

class ZipReader {
public:
    // Opens the archive and reads its central directory.
    explicit ZipReader(std::string path);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    std::optional<ZipEntry> find(const std::string& name) const;
    const std::string& path() const { return path_; }

    // Read and (if needed) decompress a whole entry into memory.
    // Only STORED (0) and DEFLATE (8) are supported -- fine for OTA metadata.
    Bytes read(const ZipEntry& e) const;

private:
    void read_central_directory();
    std::string path_;
    std::vector<ZipEntry> entries_;
};

} // namespace hw::ota
