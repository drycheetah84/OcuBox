// Qualcomm TCSR SoC-info block at 0x01fc8000 -- functional model for the real
// QSEE cold boot.
//
// tz's QSEE reads a SoC-identity block from TCSR 0x1fc8000 (memcpy + parse) during
// cold boot; a zeroed block fails its validation and QSEE parks. Supply the real
// Kona (SM8250) SoC identity so QSEE's parse succeeds. Values are the device's true
// hardware identity (faithful): soc_id 356 (0x164) = SM8250/Kona.
//
// NOTE: the exact TCSR SoC-info layout is Qualcomm-internal; the fields below are a
// best-effort reconstruction (SoC HW version @0, soc_id @4, version @8) refined
// empirically against QSEE's parser.
#pragma once
#include "devices/device.h"
#include <cstdint>

namespace hw::dev {

class QcomTcsr : public MmioDevice {
public:
    QcomTcsr(uint64_t base, uint64_t size) : base_(base), size_(size) {}
    const char* name() const override { return "tcsr_soc_info"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Stub; }

    uint64_t read(uint64_t off, unsigned) override {
        switch (off) {
            case 0x000: return 0x60040000u;   // TCSR_SOC_HW_VERSION (family/device/rev, best-effort)
            case 0x004: return 0x00000164u;   // soc_id = 356 (SM8250 / Kona)
            case 0x008: return 0x00020000u;   // version (major.minor), best-effort
            default:    return 0;
        }
    }
    void write(uint64_t, uint64_t, unsigned) override {}

private:
    uint64_t base_, size_;
};

} // namespace hw::dev
