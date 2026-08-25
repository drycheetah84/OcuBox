// Flattened Device Tree (FDT / .dtb) parser. Builds an in-memory node tree so
// the emulator can learn -- from the OS's own source of truth -- where RAM is,
// what the console UART is and at which MMIO address, the interrupt controller
// layout, and the kernel boot arguments.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hw::boot {

struct FdtProp {
    std::string name;
    Bytes data;
    size_t blob_offset = 0;   // byte offset of this property's value within the parsed blob
    uint32_t u32(size_t index = 0) const;      // big-endian cell
    uint64_t u64(size_t index = 0) const;
    std::string str() const;                    // first NUL-terminated string
    std::vector<std::string> strlist() const;   // stringlist
};

struct FdtNode {
    std::string name;                           // e.g. "memory@80000000"
    FdtNode* parent = nullptr;
    std::vector<FdtProp> props;
    std::vector<std::unique_ptr<FdtNode>> children;

    const FdtProp* prop(const std::string& n) const;
    uint32_t address_cells() const;             // inherited from parent (#address-cells)
    uint32_t size_cells() const;
    // Decoded 'reg' as (address, size) pairs using the parent's cell counts.
    std::vector<std::pair<uint64_t, uint64_t>> reg() const;
    std::string compatible() const;             // first 'compatible' string
    bool compatible_has(const std::string& s) const;
};

class Fdt {
public:
    static Fdt parse(std::span<const uint8_t> blob);

    const FdtNode* root() const { return root_.get(); }
    const FdtNode* find(const std::string& path) const;    // "/soc/serial@..."
    // Depth-first search for the first node whose 'compatible' contains `s`.
    const FdtNode* find_compatible(const std::string& s) const;

    uint32_t boot_cpuid() const { return boot_cpuid_; }

private:
    std::unique_ptr<FdtNode> root_;
    uint32_t boot_cpuid_ = 0;
};

// Return the '/'-rooted path of a node (walks parent links). "/" for the root.
std::string fdt_node_path(const FdtNode* n);

// Produce a new DTB blob with status="disabled" added to the first node matching
// `id` (an exact '/'-rooted path if it starts with '/', otherwise a substring of
// a node's `compatible`). On success sets modified=true and out_path to the
// node's path; on no-match returns an unchanged copy with modified=false. The
// blob is grown correctly (struct + strings + header sizes fixed up) so the
// original DTB is never mutated in place -- callers keep the stock blob intact.
Bytes fdt_disable(std::span<const uint8_t> blob, const std::string& id,
                  bool& modified, std::string& out_path);

// Add a property `pname`=`value` to the first node matching `id` (path or
// compatible-substring). Grows the blob correctly. Used to advertise the
// initramfs to the kernel via /chosen/linux,initrd-start / -end.
Bytes fdt_add_prop(std::span<const uint8_t> blob, const std::string& id,
                   const std::string& pname, std::span<const uint8_t> value,
                   bool& modified, std::string& out_path);

} // namespace hw::boot
