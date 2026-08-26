#include "devices/qcom_rsc.h"

namespace hw::dev {

// DRV_PRNT_CHLD_CONFIG lives at each DRV region's base+0x0C. The RSC has up to 3
// DRV regions at 0x10000 strides (drv-0/1/2). Advertise ncpt=16 (bits[31:27]) and
// num_tcs=16 (bits[17:12]) which satisfies the driver's requirement of >=8 TCS and
// 16 commands-per-TCS regardless of which DRV index it uses.
static constexpr uint32_t kDrvConfig = (16u << 27) | (16u << 12);   // 0x80010000

uint64_t QcomRsc::read(uint64_t offset, unsigned /*size*/) {
    if ((offset & 0xffff) == 0x0c && (offset >> 16) <= 2)   // per-DRV config reg
        return kDrvConfig;
    auto it = regs_.find(offset);
    return it != regs_.end() ? it->second : 0;
}

void QcomRsc::write(uint64_t offset, uint64_t value, unsigned /*size*/) {
    regs_[offset] = (uint32_t)value;   // plain storage so write-sync read-backs match
}

} // namespace hw::dev
