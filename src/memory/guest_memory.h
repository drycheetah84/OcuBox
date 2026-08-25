// Guest physical RAM. A single contiguous region based at the SoC's DRAM base
// (0x80000000 on Kona). Kept deliberately simple; MMIO is handled separately by
// the device bus so this stays a pure backing store the CPU backend can map.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <span>

namespace hw::mem {

class GuestMemory {
public:
    GuestMemory(uint64_t base, uint64_t size);

    uint64_t base() const { return base_; }
    uint64_t size() const { return ram_.size(); }
    uint64_t end() const { return base_ + ram_.size(); }
    bool contains(uint64_t gpa, uint64_t len = 1) const {
        return gpa >= base_ && gpa + len <= end() && gpa + len >= gpa;
    }

    uint8_t* host_ptr(uint64_t gpa);              // for CPU backend mapping
    std::span<uint8_t> span() { return ram_; }

    // Copy a blob into guest RAM at a guest-physical address.
    void load(uint64_t gpa, std::span<const uint8_t> data);

    uint32_t read32(uint64_t gpa) const;
    uint64_t read64(uint64_t gpa) const;   // physical read (bypasses guest MMU)
    void write32(uint64_t gpa, uint32_t v);

private:
    uint64_t base_;
    Bytes ram_;
};

} // namespace hw::mem
