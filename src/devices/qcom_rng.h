// Qualcomm PRNG (msm_rng) at 0x00793000 -- functional model.
//
// The msm_rng hwrng driver polls PRNG_STATUS (0x04) bit0 for "data available"
// then reads PRNG_DATA_OUT (0x00). Under permissive zero-RAM the status bit is
// never set, so the hwrng core kthread hot-loops logging "hwrng: no data
// available" (flooding the log and starving other work) and the kernel's crng
// never gathers hardware entropy. Report data always available and return a
// deterministic xorshift32 stream -- emulation entropy need not be cryptographic
// and determinism keeps boots reproducible.
#pragma once
#include "devices/device.h"
#include <cstdint>

namespace hw::dev {

class QcomRng : public MmioDevice {
public:
    QcomRng(uint64_t base, uint64_t size) : base_(base), size_(size) {}
    const char* name() const override { return "prng"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t off, unsigned) override {
        switch (off) {
            case 0x000: {                          // PRNG_DATA_OUT: next word
                uint32_t x = lfsr_;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                lfsr_ = x ? x : 0x1234567u;
                return lfsr_;
            }
            case 0x004: return 0x1;                // PRNG_STATUS: DATA_AVAIL
            case 0x100: return lfsr_cfg_;          // PRNG_LFSR_CFG
            case 0x104: return cfg_;               // PRNG_CONFIG (HW enable etc.)
            default:    return 0;
        }
    }
    void write(uint64_t off, uint64_t v, unsigned) override {
        if (off == 0x100)      lfsr_cfg_ = (uint32_t)v;
        else if (off == 0x104) cfg_ = (uint32_t)v;
    }

private:
    uint64_t base_, size_;
    uint32_t lfsr_ = 0xACE1u;
    uint32_t lfsr_cfg_ = 0, cfg_ = 0;
};

} // namespace hw::dev
