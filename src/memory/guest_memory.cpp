#include "memory/guest_memory.h"
#include "common/log.h"
#include <stdexcept>
#include <format>

namespace hw::mem {

GuestMemory::GuestMemory(uint64_t base, uint64_t size) : base_(base) {
    ram_.assign((size_t)size, 0);
    HW_INFO("memory", "guest RAM {} MB at {:#x}-{:#x}", size / (1024 * 1024), base_, base_ + size);
}

uint8_t* GuestMemory::host_ptr(uint64_t gpa) {
    if (!contains(gpa)) throw std::out_of_range(std::format("host_ptr {:#x} outside RAM", gpa));
    return ram_.data() + (gpa - base_);
}

void GuestMemory::load(uint64_t gpa, std::span<const uint8_t> data) {
    if (!contains(gpa, data.size()))
        throw std::out_of_range(std::format(
            "load {:#x}+{:#x} outside RAM {:#x}-{:#x}", gpa, data.size(), base_, end()));
    std::copy(data.begin(), data.end(), ram_.begin() + (gpa - base_));
    HW_DEBUG("memory", "loaded {} bytes at {:#x}", data.size(), gpa);
}

uint32_t GuestMemory::read32(uint64_t gpa) const {
    if (!contains(gpa, 4)) throw std::out_of_range("read32");
    const uint8_t* p = ram_.data() + (gpa - base_);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t GuestMemory::read64(uint64_t gpa) const {
    if (!contains(gpa, 8)) throw std::out_of_range("read64");
    const uint8_t* p = ram_.data() + (gpa - base_);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

void GuestMemory::write32(uint64_t gpa, uint32_t v) {
    if (!contains(gpa, 4)) throw std::out_of_range("write32");
    uint8_t* p = ram_.data() + (gpa - base_);
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

} // namespace hw::mem
