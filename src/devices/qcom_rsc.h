// Qualcomm RPMh RSC (Resource State Coordinator) -- minimal model.
//
// With the cmd-db STANDALONE bit set, Linux's rpmh vote paths never send TCS
// commands, so this controller only has to (a) let the rpmh-rsc driver PROBE and
// (b) satisfy the driver's write-then-read-back "sync" writes. Probe reads the
// per-DRV config register DRV_PRNT_CHLD_CONFIG at drv_base+0x0C to learn the
// number of TCS/commands; we return a value advertising >=8 TCS and >=16
// commands-per-TCS. Every other register is plain read/write storage.
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <unordered_map>

namespace hw::dev {

class QcomRsc : public MmioDevice {
public:
    QcomRsc(uint64_t base, uint64_t size, const char* name)
        : base_(base), size_(size), name_(name) {}

    const char* name() const override { return name_; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t offset, unsigned size) override;
    void write(uint64_t offset, uint64_t value, unsigned size) override;

private:
    uint64_t base_, size_;
    const char* name_;
    std::unordered_map<uint64_t, uint32_t> regs_;   // sparse RW storage (readback)
};

} // namespace hw::dev
