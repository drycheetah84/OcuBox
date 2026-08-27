// Qualcomm secure crypto/PRNG engine (EE) at 0x00791000 -- functional stub for
// the real TrustZone (tz) cold boot.
//
// tz's cold boot initializes this block by writing enable/config bits and polling
// them back (write 1 -> read-until-bit-set), reading a "data available" status
// (offset 0x04 bit0), and reading random words (offset 0x00). It is NOT a simple
// fixed-function register file: tz expects register-like readback (a value written
// is read back), so we back the whole window with storage. A few offsets are
// hardware-driven and forced:
//   0x00  DATA_OUT   -- deterministic xorshift PRNG word (entropy)
//   0x04  STATUS     -- bit0 (DATA_AVAIL/ready) forced set
// All other offsets behave as plain registers (writes stick, reads return them),
// which satisfies the "write enable bit, poll until it reads back" init loops.
//
// This models the register HANDSHAKE of the engine, not its cryptographic
// operations; if tz later drives real crypto through it and validates results,
// that will surface as a separate, deeper requirement.
#pragma once
#include "devices/device.h"
#include <array>
#include <cstdint>

namespace hw::dev {

class QcomSecEngine : public MmioDevice {
public:
    QcomSecEngine(uint64_t base, uint64_t size) : base_(base), size_(size) {}
    const char* name() const override { return "sec_engine"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Stub; }

    uint64_t read(uint64_t off, unsigned) override {
        uint32_t o = (uint32_t)(off & (size_ - 1));
        switch (o) {
            case 0x000: {                              // DATA_OUT: next PRNG word
                uint32_t x = lfsr_;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                lfsr_ = x ? x : 0x1234567u;
                return lfsr_;
            }
            case 0x004:                                // STATUS: DATA_AVAIL/ready
                return reg(o) | 0x1u;
            default:
                return reg(o);                         // register readback
        }
    }
    void write(uint64_t off, uint64_t v, unsigned) override {
        uint32_t o = (uint32_t)(off & (size_ - 1));
        if (o < kBytes) regs_[o >> 2] = (uint32_t)v;
    }

private:
    static constexpr uint32_t kBytes = 0x1000;
    uint32_t reg(uint32_t o) const { return (o < kBytes) ? regs_[o >> 2] : 0u; }

    uint64_t base_, size_;
    uint32_t lfsr_ = 0xC0FFEEu;
    std::array<uint32_t, kBytes / 4> regs_{};
};

} // namespace hw::dev
