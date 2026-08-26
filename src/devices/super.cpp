#include "devices/super.h"
#include "common/sha256.h"
#include "common/log.h"
#include <algorithm>
#include <cstring>

namespace hw::dev {
namespace {

// liblp on-disk constants (fs_mgr/liblp/include/liblp/metadata_format.h).
constexpr uint32_t LP_GEOMETRY_MAGIC   = 0x616c4467;
constexpr uint32_t LP_HEADER_MAGIC     = 0x414c5030;
constexpr uint16_t LP_MAJOR_VERSION    = 10;
constexpr uint16_t LP_MINOR_VERSION    = 1;   // group max_size + block-device flags
constexpr uint32_t LP_HEADER_SIZE      = 128;
constexpr uint32_t LP_GEOMETRY_SIZE    = 4096;
constexpr uint32_t LP_RESERVED_BYTES   = 4096;
constexpr uint32_t LP_METADATA_MAX     = 65536;
constexpr uint32_t LP_SLOT_COUNT       = 2;
constexpr uint32_t LP_BLOCK_SIZE       = 4096;
constexpr uint64_t LP_SECTOR           = 512;
constexpr uint64_t SUPER_ALIGN         = 1024 * 1024;   // place partitions 1MB-aligned

constexpr uint32_t LP_TARGET_TYPE_LINEAR    = 0;
constexpr uint32_t LP_PARTITION_ATTR_READONLY = 0x1;

void put16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
void put32(uint8_t* p, uint32_t v) { for (int i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
void put64(uint8_t* p, uint64_t v) { for (int i=0;i<8;++i) p[i]=(uint8_t)(v>>(8*i)); }
uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

} // namespace

SuperImage::SuperImage(std::vector<Logical> parts, std::string group_name, uint64_t group_max,
                       std::function<Bytes(const std::string&)> extractor)
    : parts_(std::move(parts)), extractor_(std::move(extractor)) {
    build_metadata(group_name, group_max);
}

void SuperImage::build_metadata(const std::string& group_name, uint64_t group_max) {
    // --- 1. Lay out logical partitions as 1MB-aligned LINEAR extents in super ---
    const uint64_t meta_end = LP_RESERVED_BYTES + 2 * LP_GEOMETRY_SIZE
                            + 2 * (uint64_t)LP_METADATA_MAX * LP_SLOT_COUNT;
    first_logical_byte_ = round_up(meta_end, SUPER_ALIGN);

    uint64_t off = first_logical_byte_;
    for (const auto& lp : parts_) {
        uint64_t mapped_bytes = round_up(lp.image_size, LP_BLOCK_SIZE);  // dm-linear device size
        placed_.push_back({ lp.name, lp.ota_base, off, lp.image_size, mapped_bytes / LP_SECTOR });
        off = round_up(off + mapped_bytes, SUPER_ALIGN);
    }
    total_size_ = std::max(off, first_logical_byte_ + SUPER_ALIGN);

    // --- 2. Build the metadata tables ---
    Bytes partitions((size_t)placed_.size() * 52, 0);
    Bytes extents((size_t)placed_.size() * 24, 0);
    for (size_t i = 0; i < placed_.size(); ++i) {
        uint8_t* pe = partitions.data() + i * 52;
        std::memcpy(pe, placed_[i].name.c_str(),
                    std::min<size_t>(placed_[i].name.size(), 35));   // name[36]
        put32(pe + 36, LP_PARTITION_ATTR_READONLY);   // attributes
        put32(pe + 40, (uint32_t)i);                  // first_extent_index
        put32(pe + 44, 1);                            // num_extents
        put32(pe + 48, 1);                            // group_index (1 = named group)

        uint8_t* xe = extents.data() + i * 24;
        put64(xe + 0,  placed_[i].mapped_sectors);        // num_sectors (dm-linear size)
        put32(xe + 8,  LP_TARGET_TYPE_LINEAR);            // target_type
        put64(xe + 12, placed_[i].super_off / LP_SECTOR); // target_data (physical sector in super)
        put32(xe + 20, 0);                                // target_source (block device 0)
    }

    // Two groups: index 0 = "default" (implicit), index 1 = the dynamic group.
    Bytes groups(2 * 48, 0);
    std::memcpy(groups.data() + 0 * 48, "default", 7);
    put32(groups.data() + 0 * 48 + 36, 0);            // flags
    put64(groups.data() + 0 * 48 + 40, 0);            // maximum_size = 0 (unbounded)
    std::memcpy(groups.data() + 1 * 48, group_name.c_str(),
                std::min<size_t>(group_name.size(), 35));
    put32(groups.data() + 1 * 48 + 36, 0);
    put64(groups.data() + 1 * 48 + 40, group_max);

    // One block device: "super".
    Bytes bdev(1 * 64, 0);
    put64(bdev.data() + 0,  first_logical_byte_ / LP_SECTOR);  // first_logical_sector
    put32(bdev.data() + 8,  (uint32_t)SUPER_ALIGN);            // alignment
    put32(bdev.data() + 12, 0);                                // alignment_offset
    put64(bdev.data() + 16, total_size_);                      // size (bytes)
    std::memcpy(bdev.data() + 24, "super", 5);                 // partition_name[36]
    put32(bdev.data() + 60, 0);                                // flags

    Bytes tables;
    auto append = [&](const Bytes& b){ tables.insert(tables.end(), b.begin(), b.end()); };
    append(partitions); append(extents); append(groups); append(bdev);

    uint32_t off_p = 0;
    uint32_t off_x = (uint32_t)partitions.size();
    uint32_t off_g = off_x + (uint32_t)extents.size();
    uint32_t off_b = off_g + (uint32_t)groups.size();

    // --- 3. Header (128 bytes, minor 1) ---
    Bytes header(LP_HEADER_SIZE, 0);
    put32(header.data() + 0, LP_HEADER_MAGIC);
    put16(header.data() + 4, LP_MAJOR_VERSION);
    put16(header.data() + 6, LP_MINOR_VERSION);
    put32(header.data() + 8, LP_HEADER_SIZE);
    // header_checksum [12..44) left zero for now
    put32(header.data() + 44, (uint32_t)tables.size());          // tables_size
    uint8_t tsum[32]; crypto::sha256(tables.data(), tables.size(), tsum);
    std::memcpy(header.data() + 48, tsum, 32);                   // tables_checksum
    // table descriptors: offset, num_entries, entry_size
    auto desc = [&](int at, uint32_t o, uint32_t n, uint32_t sz){
        put32(header.data()+at, o); put32(header.data()+at+4, n); put32(header.data()+at+8, sz); };
    desc(80,  off_p, (uint32_t)placed_.size(), 52);   // partitions
    desc(92,  off_x, (uint32_t)placed_.size(), 24);   // extents
    desc(104, off_g, 2, 48);                          // groups
    desc(116, off_b, 1, 64);                          // block_devices
    uint8_t hsum[32]; crypto::sha256(header.data(), header.size(), hsum);
    std::memcpy(header.data() + 12, hsum, 32);                   // header_checksum

    Bytes metadata; metadata.insert(metadata.end(), header.begin(), header.end());
    metadata.insert(metadata.end(), tables.begin(), tables.end());

    // --- 4. Geometry (52 bytes, stored in a 4096 block, twice) ---
    Bytes geom(LP_GEOMETRY_SIZE, 0);
    put32(geom.data() + 0, LP_GEOMETRY_MAGIC);
    put32(geom.data() + 4, 52);                        // struct_size
    // checksum [8..40) zero during hashing
    put32(geom.data() + 40, LP_METADATA_MAX);
    put32(geom.data() + 44, LP_SLOT_COUNT);
    put32(geom.data() + 48, LP_BLOCK_SIZE);
    uint8_t gsum[32]; crypto::sha256(geom.data(), 52, gsum);
    std::memcpy(geom.data() + 8, gsum, 32);

    // --- 5. Assemble the metadata region [0, first_logical_byte_) ---
    meta_region_.assign((size_t)first_logical_byte_, 0);
    std::memcpy(meta_region_.data() + LP_RESERVED_BYTES,                   geom.data(), LP_GEOMETRY_SIZE); // primary geom
    std::memcpy(meta_region_.data() + LP_RESERVED_BYTES + LP_GEOMETRY_SIZE, geom.data(), LP_GEOMETRY_SIZE); // backup geom
    uint64_t prim = LP_RESERVED_BYTES + 2 * LP_GEOMETRY_SIZE;
    uint64_t back = prim + (uint64_t)LP_METADATA_MAX * LP_SLOT_COUNT;
    for (uint32_t slot = 0; slot < LP_SLOT_COUNT; ++slot) {
        std::memcpy(meta_region_.data() + prim + (uint64_t)slot * LP_METADATA_MAX, metadata.data(), metadata.size());
        std::memcpy(meta_region_.data() + back + (uint64_t)slot * LP_METADATA_MAX, metadata.data(), metadata.size());
    }

    HW_WARN("super", "[LP] super metadata built: size={} MB, first_logical={} MB, {} partitions",
            total_size_ / (1024*1024), first_logical_byte_ / (1024*1024), placed_.size());
    for (const auto& p : placed_)
        HW_WARN("super", "[LP] partition {}: super_off={} MB image={} MB mapped_sectors={}",
                p.name, p.super_off / (1024*1024), p.image_size / (1024*1024), p.mapped_sectors);
}

const Bytes* SuperImage::image_for(const Placed& p) {
    auto it = cache_.find(p.name);
    if (it == cache_.end()) {
        Bytes img = extractor_ ? extractor_(p.ota_base) : Bytes{};
        it = cache_.emplace(p.name, std::move(img)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

void SuperImage::read(uint64_t off, uint64_t len, uint8_t* dst) {
    std::memset(dst, 0, (size_t)len);
    // Metadata region.
    if (off < first_logical_byte_) {
        uint64_t n = std::min(len, first_logical_byte_ - off);
        if (off < meta_region_.size())
            std::memcpy(dst, meta_region_.data() + off,
                        (size_t)std::min(n, (uint64_t)meta_region_.size() - off));
        return;   // block-aligned reads never straddle into the data region
    }
    // Logical-extent region: find the owning partition.
    for (const auto& p : placed_) {
        if (off < p.super_off || off >= p.super_off + p.mapped_sectors * LP_SECTOR) continue;
        const Bytes* img = image_for(p);
        if (img) {
            uint64_t rel = off - p.super_off;
            if (rel < img->size()) {
                uint64_t n = std::min(len, img->size() - rel);
                std::memcpy(dst, img->data() + rel, (size_t)n);
            }
        }
        return;
    }
    // Gap between partitions -> zeros (already memset).
}

} // namespace hw::dev
