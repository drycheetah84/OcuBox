// Graphics bring-up injection (enabled by EmuConfig::gfx_inject / --gfx).
//
// Strategy (no external tools, no guest binaries rebuilt beyond our own vktri):
//   * Add a "gfxsrc" GPT partition backed by a clean ext4 image that carries the
//     SwiftShader Vulkan ICD (libvk_swiftshader.so) and the vktri test program.
//   * Inject /init.hollywood.rc into the SYSTEM partition root (a single small
//     ext4 file). init imports it (import /init.${ro.hardware}.rc); it mounts
//     gfxsrc and starts vktri.
//   * Disable dm-verity (patch the vbmeta HASHTREE_DISABLED flag) so the modified
//     system image mounts. The bootloader is already presented as unlocked.
//   * vktri renders a triangle via SwiftShader and blits it to the synthetic
//     hollywood_fb MMIO device, which publishes it to the host GUI.
#pragma once
#include "common/bytes.h"
#include <string>

namespace hw::core {

// The /init.hollywood.rc script injected into the system partition root.
extern const char* const kInitHollywoodRc;

// Absolute host path of the prebuilt gfxsrc ext4 image (SwiftShader + vktri).
extern const char* const kGfxsrcImgPath;

// Size reserved for the gfxsrc GPT partition (must be >= the image on disk).
constexpr uint64_t kGfxsrcSize = 0x4000000ull;   // 64 MB

// Location of the /mnt/gfx/framebuffer file WITHIN the gfxsrc ext4 (from ext4_add
// when gfxsrc.img is built: libvk_swiftshader.so @969, vktri @9741, framebuffer
// @9746). block_size == LBA size (4096), so this is a direct partition-block offset.
constexpr uint64_t kFbFsBlock = 9746;
constexpr uint64_t kFbBlocks  = 1024;            // 4 MB (up to 1024x1024 RGBA)

// Patch an AVB vbmeta image in place to set HASHTREE_DISABLED|VERIFICATION_DISABLED
// so first-stage init skips dm-verity for all chained partitions. No-op if the
// buffer is not a "AVB0" vbmeta. Returns true if patched.
bool vbmeta_disable_verity(Bytes& vbmeta);

// Add /init.hollywood.rc (kInitHollywoodRc) to the root of an ext4 system image
// in place. Returns true on success; sets `err` otherwise.
bool inject_init_rc(Bytes& system_img, std::string& err);

// Add the composer3 VINTF fragment to the root of an ext4 vendor image, at
// /etc/vintf/manifest/questemu-composer-shim.xml. Returns true on success.
bool inject_composer_vintf(Bytes& vendor_img, std::string& err);

// Load the gfxsrc image from disk (empty if absent).
Bytes load_gfxsrc_img();

} // namespace hw::core
