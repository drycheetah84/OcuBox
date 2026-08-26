#include "devices/ufs_disk.h"
#include <algorithm>
#include <cstring>

namespace hw::dev {

namespace {
uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (uint32_t)(-(int)(c & 1)));
    }
    return ~c;
}
void put32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
void put64(uint8_t* p, uint64_t v) { for (int i=0;i<8;++i) p[i]=(uint8_t)(v>>(8*i)); }

// Partition table (name + size in bytes). A/B partitions per the Quest OTA; sizes
// are >= the OTA image sizes so full images fit. First-stage init only needs
// boot_a/dtbo_a/system_a/vbmeta_a/vendor_a to appear, but we lay out the full set.
struct Def { const char* name; uint64_t size; };
const Def kDefs[] = {
    { "vbmeta_a", 0x10000 }, { "vbmeta_b", 0x10000 },
    { "vbmeta_system_a", 0x10000 }, { "vbmeta_system_b", 0x10000 },
    { "vbmeta_vendor_a", 0x10000 }, { "vbmeta_vendor_b", 0x10000 },
    { "boot_a", 0x6400000 }, { "boot_b", 0x6400000 },           // 100M
    { "dtbo_a", 0x1000000 }, { "dtbo_b", 0x1000000 },           // 16M
    { "vendor_a", 0x14000000 }, { "vendor_b", 0x14000000 },     // 320M
    { "system_a", 0x40000000 }, { "system_b", 0x40000000 },     // 1G
    { "system_ext_a", 0x58000000 }, { "system_ext_b", 0x58000000 }, // 1.4G
    { "product_a", 0x10000000 }, { "product_b", 0x10000000 },   // 256M
    { "odm_a", 0xC000000 }, { "odm_b", 0xC000000 },             // 192M
    { "metadata", 0x1000000 }, { "misc", 0x100000 },
    { "userdata", 0x100000000ull },                            // 4G
};

// A fixed "Linux filesystem data" type GUID for every partition (the by-name
// symlink keys on the entry NAME, not the type).
const uint8_t kTypeGuid[16] = {
    0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7,
};
} // namespace

UfsDisk::UfsDisk() { build_gpt(); }

void UfsDisk::build_gpt() {
    const uint64_t entries_bytes = 128 * 128;                 // 128 entries * 128B = 16KB
    const uint64_t entry_blocks = (entries_bytes + kBlock - 1) / kBlock;  // 4 blocks @4096
    entries_lba_ = 2;
    const uint64_t align = 256;                               // 1MB alignment
    uint64_t lba = ((entries_lba_ + entry_blocks + align - 1) / align) * align;  // first usable, aligned

    for (const auto& d : kDefs) {
        uint64_t nblk = (d.size + kBlock - 1) / kBlock;
        parts_.push_back({ d.name, lba, lba + nblk - 1 });
        lba += ((nblk + align - 1) / align) * align;          // keep partitions 1MB-aligned
    }
    // Backup: entry array + header at the very end.
    backup_entries_lba_ = lba;
    backup_header_lba_ = backup_entries_lba_ + entry_blocks;
    total_blocks_ = backup_header_lba_ + 1;
    const uint64_t first_usable = entries_lba_ + entry_blocks;
    const uint64_t last_usable = backup_entries_lba_ - 1;

    // Partition entry array.
    entries_.assign((size_t)entries_bytes, 0);
    for (size_t i = 0; i < parts_.size(); ++i) {
        uint8_t* e = entries_.data() + i * 128;
        std::memcpy(e, kTypeGuid, 16);
        // unique guid: derive deterministically from index.
        std::memcpy(e + 16, kTypeGuid, 16); e[16] = (uint8_t)(i + 1);
        put64(e + 32, parts_[i].first_lba);
        put64(e + 40, parts_[i].last_lba);
        put64(e + 48, 0);                                     // attributes
        const std::string& nm = parts_[i].name;              // UTF-16LE name
        for (size_t k = 0; k < nm.size() && k < 35; ++k) e[56 + k * 2] = (uint8_t)nm[k];
    }
    uint32_t entries_crc = crc32(entries_.data(), entries_.size());

    auto build_header = [&](bool primary) {
        Bytes h((size_t)kBlock, 0);
        std::memcpy(h.data(), "EFI PART", 8);
        put32(h.data() + 8, 0x00010000);                     // revision
        put32(h.data() + 12, 92);                            // header size
        put64(h.data() + 24, primary ? 1 : backup_header_lba_);          // my_lba
        put64(h.data() + 32, primary ? backup_header_lba_ : 1);          // alternate_lba
        put64(h.data() + 40, first_usable);
        put64(h.data() + 48, last_usable);
        // disk guid (fixed)
        std::memcpy(h.data() + 56, kTypeGuid, 16); h[56] = 0xEE;
        put64(h.data() + 72, primary ? entries_lba_ : backup_entries_lba_);  // entry array lba
        put32(h.data() + 80, 128);                           // num entries
        put32(h.data() + 84, 128);                           // entry size
        put32(h.data() + 88, entries_crc);
        put32(h.data() + 16, 0);                             // crc field zero before hashing
        uint32_t hc = crc32(h.data(), 92);
        put32(h.data() + 16, hc);
        return h;
    };
    header_ = build_header(true);
    backup_header_ = build_header(false);

    // Protective MBR (first 512 bytes of LBA0): one 0xEE partition covering disk.
    mbr_.assign((size_t)kBlock, 0);
    uint8_t* pe = mbr_.data() + 446;
    pe[0] = 0x00; pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;   // start CHS
    pe[4] = 0xEE;                                             // type = GPT protective
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;                 // end CHS
    put32(pe + 8, 1);                                        // start LBA
    uint64_t sz = total_blocks_ - 1;
    put32(pe + 12, sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz);
    mbr_[510] = 0x55; mbr_[511] = 0xAA;
}

// Lazily reconstruct + cache a partition's backing image from the OTA.
const Bytes* UfsDisk::partition_data(const Part& p) {
    auto it = data_cache_.find(p.name);
    if (it == data_cache_.end()) {
        Bytes img = extractor_ ? extractor_(p.name) : Bytes{};
        it = data_cache_.emplace(p.name, std::move(img)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

void UfsDisk::read(uint64_t lba, uint32_t count, uint8_t* out) {
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t b = lba + i;
        uint8_t* dst = out + (size_t)i * kBlock;
        std::memset(dst, 0, kBlock);
        if (b == 0) {                                        // protective MBR
            std::memcpy(dst, mbr_.data(), kBlock);
        } else if (b == 1) {                                 // primary GPT header
            std::memcpy(dst, header_.data(), kBlock);
        } else if (b >= entries_lba_ && b < entries_lba_ + (entries_.size() + kBlock - 1) / kBlock) {
            size_t off = (size_t)(b - entries_lba_) * kBlock;
            size_t n = off < entries_.size() ? std::min((size_t)kBlock, entries_.size() - off) : 0;
            if (n) std::memcpy(dst, entries_.data() + off, n);
        } else if (b == backup_header_lba_) {
            std::memcpy(dst, backup_header_.data(), kBlock);
        } else if (b >= backup_entries_lba_ && b < backup_header_lba_) {
            size_t off = (size_t)(b - backup_entries_lba_) * kBlock;
            size_t n = off < entries_.size() ? std::min((size_t)kBlock, entries_.size() - off) : 0;
            if (n) std::memcpy(dst, entries_.data() + off, n);
        } else {
            // Partition data region: serve the OTA image for whichever partition
            // owns this block (unmapped/inactive slots stay zero).
            for (const auto& p : parts_) {
                if (b < p.first_lba || b > p.last_lba) continue;
                const Bytes* img = partition_data(p);
                if (img) {
                    size_t off = (size_t)(b - p.first_lba) * kBlock;
                    size_t n = off < img->size() ? std::min((size_t)kBlock, img->size() - off) : 0;
                    if (n) std::memcpy(dst, img->data() + off, n);
                }
                break;
            }
        }
    }
}

} // namespace hw::dev
