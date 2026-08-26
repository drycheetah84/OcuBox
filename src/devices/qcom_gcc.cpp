#include "devices/qcom_gcc.h"

namespace hw::dev {

namespace {
constexpr uint32_t BIT0  = 1u << 0;    // RCG CMD_UPDATE / branch enable / GDSC SW_COLLAPSE
constexpr uint32_t BIT1  = 1u << 1;    // branch hw_ctl enable
constexpr uint32_t BIT30 = 1u << 30;   // PLL_ACTIVE_FLAG
constexpr uint32_t BIT31 = 1u << 31;   // RCG ROOT_OFF / branch CLK_OFF / GDSC PWR_ON / PLL_LOCK_DET

constexpr uint64_t GPLL0_MODE = 0x0;   // GPLL0 PLL_MODE (assume XBL left it locked+active)
} // namespace

QcomGcc::QcomGcc(uint64_t base, uint64_t size) : base_(base), size_(size) {
    // UFS RCG CMD_RCGR registers (BIT0 CMD_UPDATE self-clears).
    rcg_ = { 0x77024, 0x7706c, 0x77084, 0x770a0 };
    // UFS GDSC GDSCR (PWR_ON = !SW_COLLAPSE). Only the UFS PHY GDSC is on our path.
    gdsc_ = { 0x77004 };
}

uint64_t QcomGcc::read(uint64_t offset, unsigned /*size*/) {
    auto it = regs_.find(offset);
    uint32_t v = it != regs_.end() ? it->second : 0;

    if (offset == GPLL0_MODE)                    // GPLL0: active + locked
        return v | BIT30 | BIT31;
    if (gdsc_.count(offset))                     // GDSCR: PWR_ON(BIT31) = !SW_COLLAPSE(BIT0)
        return (v & ~BIT31) | ((v & BIT0) ? 0 : BIT31);
    // Clock branch CBCRs (offsets in the UFS window ending 0x1x/0x2x/0x6x etc.): the
    // driver polls CLK_OFF(BIT31) after toggling enable(BIT0)/hw_ctl(BIT1). Model
    // CLK_OFF = !enable so both the enable (want 0) and disable (want 1) polls pass.
    // Applied only to the UFS branch window so unrelated status regs are untouched.
    switch (offset) {
        case 0x77018: case 0x77010: case 0x77064: case 0x7705c: case 0x7709c:
        case 0x77020: case 0x770b8: case 0x7701c: case 0x770cc:
            return (v & ~BIT31) | ((v & (BIT0 | BIT1)) ? 0 : BIT31);
        default: break;
    }
    return v;
}

void QcomGcc::write(uint64_t offset, uint64_t value, unsigned /*size*/) {
    uint32_t v = (uint32_t)value;
    if (rcg_.count(offset)) v &= ~BIT0;          // CMD_UPDATE completes immediately
    regs_[offset] = v;
}

} // namespace hw::dev
