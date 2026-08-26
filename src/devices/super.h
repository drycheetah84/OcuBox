// Synthesized Android "super" dynamic-partition image (liblp).
//
// The Quest OTA ships no super.img; update_engine builds super from the per-
// partition images plus the manifest's dynamic_partition_metadata. We do the
// same: lay out the logical partitions (system_a, vendor_a, ...) as LINEAR
// extents inside a physical super partition, and synthesize the liblp geometry
// + metadata (v1.1, SHA-256 checksummed) that the real fs_mgr/liblp reads to
// create the dm-linear devices. Extent data is served on demand from the OTA.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hw::dev {

class SuperImage {
public:
    struct Logical {
        std::string name;       // liblp partition name, e.g. "system_a"
        std::string ota_base;   // OTA payload partition, e.g. "system"
        uint64_t    image_size; // bytes of real image (ext4 + hashtree)
    };

    // extractor(ota_base) -> full partition image (cached; empty => zeros).
    SuperImage(std::vector<Logical> parts, std::string group_name, uint64_t group_max,
               std::function<Bytes(const std::string&)> extractor);

    uint64_t size() const { return total_size_; }        // physical super size (bytes)
    void read(uint64_t off, uint64_t len, uint8_t* dst); // serve super bytes

    struct Placed { std::string name, ota_base; uint64_t super_off, image_size, mapped_sectors; };
    const std::vector<Placed>& layout() const { return placed_; }

private:
    void build_metadata(const std::string& group_name, uint64_t group_max);
    const Bytes* image_for(const Placed& p);

    std::vector<Logical> parts_;
    std::vector<Placed>  placed_;
    Bytes    meta_region_;      // [0, first_logical_byte_) synthesized liblp header area
    uint64_t first_logical_byte_ = 0;
    uint64_t total_size_ = 0;
    std::function<Bytes(const std::string&)> extractor_;
    std::unordered_map<std::string, Bytes> cache_;
};

} // namespace hw::dev
