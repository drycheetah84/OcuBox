#include "devices/ufs_disk.h"
#include "devices/super.h"
#include "gui/gui_bridge.h"
#include "common/log.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

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

// Physical partition table (name + size in bytes). The Quest uses DYNAMIC
// partitions: system/system_ext/vendor/product/odm are logical, living inside
// the physical `super` partition (see SuperImage); they are NOT listed here.
// `super` is sized from the SuperImage at build time. vbmeta*/boot/dtbo are real
// static A/B partitions; metadata/misc/userdata are static scratch.
struct Def { const char* name; uint64_t size; };
const Def kDefs[] = {
    { "vbmeta_a", 0x10000 }, { "vbmeta_b", 0x10000 },
    { "vbmeta_system_a", 0x10000 }, { "vbmeta_system_b", 0x10000 },
    { "vbmeta_vendor_a", 0x10000 }, { "vbmeta_vendor_b", 0x10000 },
    { "boot_a", 0x6400000 }, { "boot_b", 0x6400000 },           // 100M
    { "dtbo_a", 0x1000000 }, { "dtbo_b", 0x1000000 },           // 16M
    { "super", 0 },                                            // size from SuperImage
    { "metadata", 0x1000000 }, { "misc", 0x100000 },
    { "userdata", 0x100000000ull },                            // 4G
    // Device-provisioned (not in the OTA) partitions the secure daemons open
    // directly. vendor.qseecomd opens by-name/ssd (O_SYNC) for its SSD/secure-
    // storage init and exits 255 if absent; provide it (served as zeroed scratch).
    { "ssd", 0x100000 },
    // Firmware + QSEE trusted-app partitions (OTA-backed). fs_mgr mounts
    // /vendor/firmware_mnt from `modem` ("Skipping .../by-name/modem_a during
    // mount_all" when absent); that mount holds the QSEE TA images that
    // libQSEEComAPI loads as /vendor/firmware_mnt/image/keymaster64.mdt etc.
    // keymaster/cmnlib/cmnlib64 are the trusted-app image partitions. The "_a"
    // active slot maps to the OTA base image (extractor strips the suffix); "_b"
    // inactive slots serve zero. Without these, keymaster's TA never loads ->
    // IKeymasterDevice/keystore2 never register -> /data metadata-encryption blocked.
    { "modem_a", 0x3000000 }, { "modem_b", 0x3000000 },        // 48M (>= OTA ~41M)
    { "keymaster_a", 0x100000 }, { "keymaster_b", 0x100000 },
    { "cmnlib_a", 0x100000 }, { "cmnlib_b", 0x100000 },
    { "cmnlib64_a", 0x100000 }, { "cmnlib64_b", 0x100000 },
    // Graphics bring-up payload (--gfx): a clean ext4 image carrying the
    // SwiftShader Vulkan ICD + vktri, mounted by /init.hollywood.rc. Served from
    // a host file (not the OTA); when --gfx is off it reads back as zeros (unused).
    { "gfxsrc", 0x4000000 },                                   // 64M
};

// A fixed "Linux filesystem data" type GUID for every partition (the by-name
// symlink keys on the entry NAME, not the type).
const uint8_t kTypeGuid[16] = {
    0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7,
};
} // namespace

UfsDisk::UfsDisk(std::shared_ptr<SuperImage> super) : super_(std::move(super)) { build_gpt(); }

void UfsDisk::build_gpt() {
    const uint64_t entries_bytes = 128 * 128;                 // 128 entries * 128B = 16KB
    const uint64_t entry_blocks = (entries_bytes + kBlock - 1) / kBlock;  // 4 blocks @4096
    entries_lba_ = 2;
    const uint64_t align = 256;                               // 1MB alignment
    uint64_t lba = ((entries_lba_ + entry_blocks + align - 1) / align) * align;  // first usable, aligned

    for (const auto& d : kDefs) {
        uint64_t sz = d.size;
        if (std::string(d.name) == "super") {
            if (!super_) continue;                            // no dynamic partitions
            sz = super_->size();
        }
        uint64_t nblk = (sz + kBlock - 1) / kBlock;
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
        const std::string& nm = parts_[i].name;              // UTF-16LE name
        // A/B slot attributes (Qualcomm GPT layout in attr bits 48-63: 48-49 priority,
        // 50 active, 51-53 retry count, 54 successful, 55 unbootable). Present slot A
        // as the active+successful slot and slot B as a valid lower-priority fallback,
        // so bootctrl.<soc> (the IBootControl HAL) can resolve the current slot from
        // the GPT -- matching androidboot.slot_suffix=_a. Non-slotted partitions: 0.
        uint64_t attr = 0;
        if (nm.size() >= 2 && nm.compare(nm.size() - 2, 2, "_a") == 0)
            attr = (3ull << 48) | (1ull << 50) | (7ull << 51) | (1ull << 54);
        else if (nm.size() >= 2 && nm.compare(nm.size() - 2, 2, "_b") == 0)
            attr = (2ull << 48) | (7ull << 51);
        put64(e + 48, attr);                                  // attributes
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
        // Copy-on-write overlay: a previously-written block overrides the base image
        // (GPT/OTA/zeros). This is what makes formats + file writes persist.
        auto ov = overlay_.find(b);
        if (ov != overlay_.end()) { std::memcpy(dst, ov->second.data(), kBlock); continue; }
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
            // Partition data region.
            for (const auto& p : parts_) {
                if (b < p.first_lba || b > p.last_lba) continue;
                if (p.name == "super" && super_) {
                    // Dynamic-partition container: liblp metadata + logical extents.
                    super_->read((b - p.first_lba) * (uint64_t)kBlock, kBlock, dst);
                } else {
                    // Static partition: serve its OTA image (inactive slots stay zero).
                    const Bytes* img = partition_data(p);
                    if (img) {
                        size_t off = (size_t)(b - p.first_lba) * kBlock;
                        size_t n = off < img->size() ? std::min((size_t)kBlock, img->size() - off) : 0;
                        if (n) std::memcpy(dst, img->data() + off, n);
                    }
                }
                break;
            }
        }
    }
}

void UfsDisk::write(uint64_t lba, uint32_t count, const uint8_t* data) {
    for (uint32_t i = 0; i < count; ++i) {
        Bytes& blk = overlay_[lba + i];
        if (blk.size() != kBlock) blk.assign(kBlock, 0);
        std::memcpy(blk.data(), data + (size_t)i * kBlock, kBlock);
    }
    // --gfx: a write touching the framebuffer header block may complete a frame.
    if (fb_lba_ && lba <= fb_lba_ && fb_lba_ < lba + count) publish_framebuffer();
}

// Decode /mnt/gfx/framebuffer (header + RGBA pixels) from the overlay-backed disk
// and publish it to the host GUI. Triggered when the guest writes the header block.
void UfsDisk::publish_framebuffer() {
    uint8_t hdr[kBlock];
    read(fb_lba_, 1, hdr);                                  // overlay-aware
    if (std::memcmp(hdr, "HFB1", 4) != 0) return;
    uint32_t gen, w, h;
    std::memcpy(&gen, hdr + 4, 4); std::memcpy(&w, hdr + 8, 4); std::memcpy(&h, hdr + 12, 4);
    if (gen == fb_gen_ || w == 0 || h == 0 || w > 4096 || h > 4096) return;
    size_t pxbytes = (size_t)w * h * 4;
    if (16 + pxbytes > fb_blocks_ * (uint64_t)kBlock) return;
    uint32_t nb = (uint32_t)((16 + pxbytes + kBlock - 1) / kBlock);
    std::vector<uint8_t> buf((size_t)nb * kBlock);
    read(fb_lba_, nb, buf.data());
    const uint8_t* src = buf.data() + 16;
    auto& b = hw::gui::bridge();
    {
        std::lock_guard<std::mutex> g(b.fb_mu);
        b.fb.resize((size_t)w * h);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t r = src[i*4+0], gg = src[i*4+1], bl = src[i*4+2], a = src[i*4+3];
            b.fb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | bl;
        }
        b.fb_w = (int)w; b.fb_h = (int)h;
        b.fb_gen.fetch_add(1, std::memory_order_release);
    }
    fb_gen_ = gen;
    // Verification dump (host-side): write the captured frame as a PPM so a
    // headless run can confirm the actual pixels.
    if (std::FILE* f = std::fopen("D:\\gfxbuild\\captured_frame.ppm", "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            std::fputc(src[i*4+0], f); std::fputc(src[i*4+1], f); std::fputc(src[i*4+2], f);
        }
        std::fclose(f);
    }
    HW_WARN("hollywood_fb", "captured frame gen={} {}x{} ({} px) -> GUI + captured_frame.ppm",
            gen, w, h, (unsigned long long)((uint64_t)w * h));
}

} // namespace hw::dev
