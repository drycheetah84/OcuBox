#include "devices/device_bus.h"
#include "common/log.h"

namespace hw::dev {

void DeviceBus::add(std::unique_ptr<MmioDevice> dev) {
    HW_DEBUG("bus", "map {} at {:#x}+{:#x} [{}]", dev->name(), dev->base(), dev->size(),
             status_str(dev->status()));
    devices_.push_back(std::move(dev));
}

MmioDevice* DeviceBus::device_at(uint64_t addr) const {
    for (const auto& d : devices_)
        if (addr >= d->base() && addr < d->base() + d->size()) return d.get();
    return nullptr;
}

uint64_t DeviceBus::read(uint64_t addr, unsigned size) {
    if (MmioDevice* d = device_at(addr)) return d->read(addr - d->base(), size);
    record_unclaimed(addr, false, 0, size);
    return 0;
}

void DeviceBus::write(uint64_t addr, uint64_t value, unsigned size) {
    if (MmioDevice* d = device_at(addr)) { d->write(addr - d->base(), value, size); return; }
    record_unclaimed(addr, true, value, size);
}

void DeviceBus::record_unclaimed(uint64_t addr, bool w, uint64_t v, unsigned size) {
    for (auto& u : unclaimed_) {
        if (u.addr == addr && u.is_write == w) { u.count++; u.value = v; return; }
    }
    unclaimed_.push_back({ addr, w, v, size, 1 });
    HW_WARN("bus", "unclaimed MMIO {} {:#x} (size {}){}", w ? "write" : "read", addr, size,
            w ? "" : " -> 0");
}

} // namespace hw::dev
