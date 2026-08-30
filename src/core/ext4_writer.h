// Minimal ext4 injector used by the graphics bring-up (--gfx). Adds one regular
// file to the ROOT directory of an ext4 image held in memory. Supports the exact
// feature set the Quest images use (extents, uninit_bg/gdt_csum, dir_index,
// shared_blocks) but NOT: 64bit descriptors, metadata_csum, HTree *root* dirs, or
// files needing a non-contiguous allocation. Validated against system.img with a
// 7-Zip ext4 read-back. See ext4tool/ext4_add.cpp for the standalone/testable form.
#pragma once
#include "common/bytes.h"
#include <string>
#include <vector>

namespace hw::core {

// Add `content` as /<name> (root-level, e.g. "init.hollywood.rc") to the ext4
// image `img` (modified in place). Uses existing free space; the image size is
// unchanged. Returns false + sets `err` on failure (no free space/inode, root dir
// is HTree/full, unsupported feature, etc.).
bool ext4_add_root_file(Bytes& img, const std::string& name,
                        const std::vector<uint8_t>& content, uint32_t mode,
                        std::string& err);

// Add `content` at an absolute path (e.g. "/etc/vintf/manifest/x.xml") whose
// PARENT directory already exists and whose LEAF directory is single-block
// (non-HTree). Intermediate directories may be HTree (lookup scans leaf blocks).
bool ext4_add_file(Bytes& img, const std::string& path,
                   const std::vector<uint8_t>& content, uint32_t mode,
                   std::string& err);

// Overwrite an EXISTING file's contents in place. The file must be contiguous and
// the new `content` must fit within its current block allocation (its size may
// grow up to the slack in the last block). Updates i_size; allocation is unchanged.
bool ext4_overwrite_file(Bytes& img, const std::string& path,
                         const std::vector<uint8_t>& content, std::string& err);

} // namespace hw::core
