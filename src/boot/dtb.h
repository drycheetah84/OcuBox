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

} // namespace hw::boot
