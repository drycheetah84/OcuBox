// Address-decoded MMIO bus. The CPU backend routes any access that misses guest
// RAM here; unclaimed accesses are logged (a prime source of "what hardware is
// the kernel touching next" diagnostics during bring-up).
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace hw::dev {

struct UnclaimedAccess {
    uint64_t addr = 0;
    bool     is_write = false;
    uint64_t value = 0;
    unsigned size = 0;
    uint64_t count = 0;
};

class DeviceBus {
public:
    void add(std::unique_ptr<MmioDevice> dev);
    MmioDevice* device_at(uint64_t addr) const;
    const std::vector<std::unique_ptr<MmioDevice>>& devices() const { return devices_; }

    uint64_t read(uint64_t addr, unsigned size);
    void write(uint64_t addr, uint64_t value, unsigned size);

    const std::vector<UnclaimedAccess>& unclaimed() const { return unclaimed_; }

private:
    void record_unclaimed(uint64_t addr, bool w, uint64_t v, unsigned size);
    std::vector<std::unique_ptr<MmioDevice>> devices_;
    std::vector<UnclaimedAccess> unclaimed_;
};

} // namespace hw::dev
