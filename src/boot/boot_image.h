// Android boot image parser (header versions 0-2, which is what the Quest 2
// 'boot' partition uses -- there is no vendor_boot, and the DTB is carried in
// the v2 'dtb' area). Produces views of the kernel / ramdisk / dtb sections
// plus the load addresses and kernel command line the bootloader would use.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <span>
#include <string>

namespace hw::boot {

struct BootImage {
    uint32_t header_version = 0;
    uint32_t page_size = 0;
    uint32_t os_version_patch = 0;
    std::string board_name;
    std::string cmdline;          // cmdline + extra_cmdline concatenated

    uint64_t kernel_addr = 0;
    uint64_t ramdisk_addr = 0;
    uint64_t second_addr = 0;
    uint64_t tags_addr = 0;       // where the DTB is expected in RAM
    uint64_t dtb_addr = 0;

    std::span<const uint8_t> kernel;
    std::span<const uint8_t> ramdisk;
    std::span<const uint8_t> second;
    std::span<const uint8_t> dtb;  // embedded device tree (v2)

    // ARM64 'Image' header fields (from the kernel image itself).
    bool     kernel_is_arm64 = false;
    uint64_t kernel_text_offset = 0;  // Image header field: load offset from 2MB base
    uint64_t kernel_image_size = 0;   // Image header field: total image size

    // Holds the backing buffer so the spans stay valid.
    Bytes storage;
};

// Parse a full 'boot' partition image (takes ownership of the buffer).
BootImage parse_boot_image(Bytes image);

} // namespace hw::boot
