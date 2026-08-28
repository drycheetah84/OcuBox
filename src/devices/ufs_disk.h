// Backing storage for the emulated UFS controller: a single logical unit whose
// sectors are a synthesized GPT (protective MBR + primary/backup GPT header +
// partition entry array) describing the Quest's A/B partitions, with each
// partition's data served on demand from the Quest OTA (or zeros until wired).
//
// Block reads are answered by sector; the GPT sectors are generated in-memory and
// the partition regions map to OTA-extracted images. Logical block size 4096.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hw::dev {

class SuperImage;   // dynamic-partition (super) backing

class UfsDisk {
public:
    static constexpr uint32_t kBlock = 4096;   // logical block size

    // If `super` is set, a physical "super" partition of super->size() bytes is
    // added to the GPT and its blocks are served from the liblp image; the
    // logical partitions (system/vendor/...) live inside it. If null, the disk
    // exposes static partitions only.
    explicit UfsDisk(std::shared_ptr<SuperImage> super = nullptr);

    uint64_t block_count() const { return total_blocks_; }
    uint32_t block_size() const { return kBlock; }

    // Read `count` logical blocks starting at `lba` into out (count*kBlock bytes).
    void read(uint64_t lba, uint32_t count, uint8_t* out);

    // Write `count` logical blocks starting at `lba` from `data` (count*kBlock bytes).
    // Persisted in a copy-on-write overlay (per-LBA blocks) so the read-only OTA base
    // images are never mutated; reads consult the overlay first. This makes scratch
    // partitions (metadata/userdata/misc/ssd) genuinely writable, which the guest needs
    // to format + mount /metadata and /data (vold's metadata-encryption key lives in
    // /metadata/vold; without persistence /data reformats forever).
    void write(uint64_t lba, uint32_t count, const uint8_t* data);

    struct Part { std::string name; uint64_t first_lba, last_lba; };
    const std::vector<Part>& parts() const { return parts_; }

    // Install a backing-data source: given a GPT partition name (e.g. "vendor_a")
    // it returns that partition's full raw image (empty => served as zeros). The
    // result is cached per partition and read on demand, so only partitions the
    // guest actually touches are reconstructed from the OTA.
    using Extractor = std::function<Bytes(const std::string& part_name)>;
    void set_partition_source(Extractor ex) { extractor_ = std::move(ex); }

private:
    void build_gpt();
    const Bytes* partition_data(const Part& p);   // lazily extract + cache

    Extractor extractor_;
    std::unordered_map<std::string, Bytes> data_cache_;
    std::unordered_map<uint64_t, Bytes> overlay_;   // LBA -> written 4KB block (copy-on-write)
    std::shared_ptr<SuperImage> super_;

    std::vector<Part> parts_;
    Bytes gpt_primary_;      // LBA1 header + entries (LBA2..), and protective MBR (LBA0)
    Bytes mbr_;              // LBA0
    Bytes entries_;          // partition entry array
    Bytes header_;           // primary GPT header (LBA1)
    Bytes backup_header_;    // backup GPT header (last LBA)
    uint64_t total_blocks_ = 0;
    uint64_t entries_lba_ = 2;
    uint64_t backup_entries_lba_ = 0;
    uint64_t backup_header_lba_ = 0;
};

} // namespace hw::dev
