#include "boot/boot_image.h"
#include "common/log.h"
#include <stdexcept>
#include <format>

namespace hw::boot {

namespace {
constexpr char kMagic[8] = { 'A','N','D','R','O','I','D','!' };

uint64_t round_up(uint64_t x, uint64_t page) { return page ? ((x + page - 1) / page) * page : x; }

// ARM64 Linux 'Image' header (arch/arm64/kernel/head.S).
struct Arm64Header {
    uint32_t code0;
    uint32_t code1;
    uint64_t text_offset;   // legacy load offset (0 when image_size is used)
    uint64_t image_size;
    uint64_t flags;
    uint64_t res2, res3, res4;
    uint32_t magic;         // 0x644d5241 = "ARM\x64"
    uint32_t res5;
};
} // namespace

BootImage parse_boot_image(Bytes image) {
    BootImage bi;
    bi.storage = std::move(image);
    std::span<const uint8_t> buf(bi.storage);
    if (buf.size() < 4096 || std::memcmp(buf.data(), kMagic, 8) != 0)
        throw std::runtime_error("not an Android boot image (missing ANDROID! magic)");

    ByteReader r(buf);
    r.skip(8); // magic
    uint32_t kernel_size = r.u32le();
    bi.kernel_addr       = r.u32le();
    uint32_t ramdisk_size= r.u32le();
    bi.ramdisk_addr      = r.u32le();
    uint32_t second_size = r.u32le();
    bi.second_addr       = r.u32le();
    bi.tags_addr         = r.u32le();
    bi.page_size         = r.u32le();
    bi.header_version    = r.u32le();
    bi.os_version_patch  = r.u32le();
    bi.board_name        = read_cstr(buf.subspan(48, 16));
    std::string cmdline  = read_cstr(buf.subspan(64, 512));
    // id[8] at 576; extra_cmdline at 608
    std::string extra    = read_cstr(buf.subspan(608, 1024));
    bi.cmdline = cmdline;
    if (!extra.empty()) { if (!bi.cmdline.empty()) bi.cmdline += " "; bi.cmdline += extra; }

    uint32_t recovery_dtbo_size = 0, dtb_size = 0;
    if (bi.header_version >= 1) {
        ByteReader v(buf); v.seek(1632);
        recovery_dtbo_size = v.u32le();
        v.skip(8);          // recovery_dtbo_offset
        v.skip(4);          // header_size
        if (bi.header_version >= 2) {
            dtb_size = v.u32le();
            bi.dtb_addr = v.u64le();
        }
    }

    const uint32_t p = bi.page_size;
    if (p == 0 || (p & (p - 1))) throw std::runtime_error("bad boot image page size");

    uint64_t off = p; // sections start after the 1-page header
    bi.kernel = buf.subspan(off, kernel_size); off += round_up(kernel_size, p);
    if (ramdisk_size) { bi.ramdisk = buf.subspan(off, ramdisk_size); }
    off += round_up(ramdisk_size, p);
    if (second_size) { bi.second = buf.subspan(off, second_size); }
    off += round_up(second_size, p);
    off += round_up(recovery_dtbo_size, p);
    if (dtb_size) { bi.dtb = buf.subspan(off, dtb_size); }

    // Inspect the ARM64 kernel image header.
    if (bi.kernel.size() >= sizeof(Arm64Header)) {
        Arm64Header h{};
        std::memcpy(&h, bi.kernel.data(), sizeof(h));
        if (h.magic == 0x644d5241u) {
            bi.kernel_is_arm64 = true;
            bi.kernel_text_offset = h.text_offset;
            bi.kernel_image_size = h.image_size;
        }
    }

    HW_INFO("boot.img", "hdr v{} page={} board='{}' kernel={}KB ramdisk={}KB dtb={}KB",
            bi.header_version, bi.page_size, bi.board_name,
            kernel_size / 1024, ramdisk_size / 1024, dtb_size / 1024);
    HW_INFO("boot.img", "addrs kernel={:#x} ramdisk={:#x} tags={:#x} dtb={:#x}",
            bi.kernel_addr, bi.ramdisk_addr, bi.tags_addr, bi.dtb_addr);
    HW_INFO("boot.img", "arm64={} text_offset={:#x} image_size={:#x}",
            bi.kernel_is_arm64, bi.kernel_text_offset, bi.kernel_image_size);
    HW_INFO("boot.img", "cmdline: {}", bi.cmdline);
    return bi;
}

} // namespace hw::boot
