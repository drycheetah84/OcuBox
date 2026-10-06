// Guest physical RAM. A single contiguous region based at the SoC's DRAM base
// (0x80000000 on Kona). Kept deliberately simple; MMIO is handled separately by
// the device bus so this stays a pure backing store the CPU backend can map.
#pragma once
#include "common/bytes.h"
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>

namespace hw::mem {

class GuestMemory {
public:
    GuestMemory(uint64_t base, uint64_t size);

    uint64_t base() const { return base_; }
    uint64_t size() const { return size_; }
    uint64_t end() const { return base_ + size_; }
    bool contains(uint64_t gpa, uint64_t len = 1) const {
        return gpa >= base_ && gpa + len <= end() && gpa + len >= gpa;
    }

    uint8_t* host_ptr(uint64_t gpa);              // for CPU backend mapping
    std::span<uint8_t> span() { return { ram_.get(), (size_t)size_ }; }

    // Copy a blob into guest RAM at a guest-physical address.
    void load(uint64_t gpa, std::span<const uint8_t> data);

    uint32_t read32(uint64_t gpa) const;
    uint64_t read64(uint64_t gpa) const;   // physical read (bypasses guest MMU)
    void write32(uint64_t gpa, uint32_t v);

private:
    uint64_t base_;
    uint64_t size_;
    // calloc'd rather than a zero-filled vector: large allocations come straight
    // from the OS as zero pages, so untouched guest RAM costs neither time nor
    // host memory (a vector memsets -- and commits -- every byte up front).
    struct FreeDeleter { void operator()(uint8_t* p) const { std::free(p); } };
    std::unique_ptr<uint8_t[], FreeDeleter> ram_;
};

} // namespace hw::mem
