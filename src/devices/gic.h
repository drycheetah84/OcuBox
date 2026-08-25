// ARM GICv3 interrupt controller (distributor + redistributor MMIO frames).
//
// Models enough of the register interface for the Linux irq-gic-v3 driver to
// probe and initialize: the ID registers that identify a GICv3 (GICD_PIDR2 /
// GICR_PIDR2 ArchRev=3), GICD_TYPER (interrupt count), the redistributor
// GICR_TYPER (with the Last bit so the driver stops after CPU0) and GICR_WAKER
// (ProcessorSleep/ChildrenAsleep handshake). Interrupt delivery itself is not
// modeled yet (no IRQ lines are asserted), which is fine for boot bring-up.
#pragma once
#include "devices/device.h"
#include <algorithm>
#include <cstdint>

namespace hw::dev {

class GicV3 : public MmioDevice {
public:
    GicV3(uint64_t dist_base, uint64_t dist_size,
          uint64_t redist_base, uint64_t redist_size)
        : dist_base_(dist_base), dist_size_(dist_size),
          redist_base_(redist_base), redist_size_(redist_size) {}

    const char* name() const override { return "gic-v3"; }
    uint64_t base() const override { return dist_base_ < redist_base_ ? dist_base_ : redist_base_; }
    uint64_t size() const override {
        uint64_t lo = base();
        uint64_t hi = std::max(dist_base_ + dist_size_, redist_base_ + redist_size_);
        return hi - lo;
    }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t offset, unsigned size) override;
    void write(uint64_t offset, uint64_t value, unsigned size) override;

private:
    uint64_t dist_read(uint64_t off, unsigned size);
    void     dist_write(uint64_t off, uint64_t value, unsigned size);
    uint64_t redist_read(uint64_t off, unsigned size);
    void     redist_write(uint64_t off, uint64_t value, unsigned size);

    uint64_t dist_base_, dist_size_, redist_base_, redist_size_;
    uint32_t gicd_ctlr_ = 0;
    uint32_t gicr_waker_ = 0x6;   // ProcessorSleep(1) | ChildrenAsleep(2) at reset
};

} // namespace hw::dev
