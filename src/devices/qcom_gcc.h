// Qualcomm Global Clock Controller (GCC) -- minimal functional model.
//
// gcc-kona registers hundreds of clocks/resets/GDSCs in the 0x100000 window. The
// driver enables the ones a consumer (e.g. UFS) needs by poking these registers
// and polling a status bit. With permissive zero-RAM those polls never resolve
// ("rcg didn't update", GDSC power-on timeout). We model just the four bit
// behaviours the bring-up polls on (see qcom_platform_research.md):
//   * RCG update:  CMD_RCGR BIT0 (CMD_UPDATE) must self-clear after a write.
//   * Branch:      CBCR BIT31 (CLK_OFF) reads 0 when enabled (default: returns
//                  the stored value, whose BIT31 stays 0 -> enable poll passes).
//   * GDSC power:  GDSCR BIT31 (PWR_ON) = !BIT0 (SW_COLLAPSE).
//   * PLL lock:    GPLL0 PLL_MODE(@0x0) reads bits 30+31 set (active+locked).
// Everything else is plain read/write storage. Offsets are GCC-window relative.
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace hw::dev {

class QcomGcc : public MmioDevice {
public:
    QcomGcc(uint64_t base, uint64_t size);

    const char* name() const override { return "gcc"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t offset, unsigned size) override;
    void write(uint64_t offset, uint64_t value, unsigned size) override;

private:
    uint64_t base_, size_;
    std::unordered_map<uint64_t, uint32_t> regs_;   // sparse RW storage
    std::unordered_set<uint64_t> rcg_;              // CMD_RCGR offsets (BIT0 self-clear)
    std::unordered_set<uint64_t> gdsc_;             // GDSCR offsets (PWR_ON=!SW_COLLAPSE)
};

} // namespace hw::dev
