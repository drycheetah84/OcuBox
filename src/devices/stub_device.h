// A placeholder MMIO region that absorbs accesses (reads return 0) so the guest
// doesn't fault on hardware we haven't modeled yet. Every access is counted so
// diagnostics can show exactly how hard the kernel is leaning on it -- these are
// the natural candidates for the next real device implementation.
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <string>

namespace hw::dev {

class StubDevice : public MmioDevice {
public:
    StubDevice(std::string name, uint64_t base, uint64_t size)
        : name_(std::move(name)), base_(base), size_(size) {}

    const char* name() const override { return name_.c_str(); }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Stub; }

    uint64_t read(uint64_t, unsigned) override { reads_++; return 0; }
    void write(uint64_t, uint64_t, unsigned) override { writes_++; }

    uint64_t reads() const { return reads_; }
    uint64_t writes() const { return writes_; }

private:
    std::string name_;
    uint64_t base_, size_;
    uint64_t reads_ = 0, writes_ = 0;
};

} // namespace hw::dev
