#include "devices/gic.h"
#include "common/log.h"
#include <algorithm>

namespace hw::dev {

namespace {
// Distributor registers.
constexpr uint64_t GICD_CTLR   = 0x0000;
constexpr uint64_t GICD_TYPER  = 0x0004;
constexpr uint64_t GICD_IIDR   = 0x0008;
constexpr uint64_t GICD_PIDR2  = 0xFFE8;
// Redistributor RD frame registers.
constexpr uint64_t GICR_CTLR   = 0x0000;
constexpr uint64_t GICR_IIDR   = 0x0004;
constexpr uint64_t GICR_TYPER  = 0x0008;   // 64-bit
constexpr uint64_t GICR_WAKER  = 0x0014;
constexpr uint64_t GICR_PIDR2  = 0xFFE8;

constexpr uint64_t kRdFrameStride = 0x20000; // RD (0x10000) + SGI (0x10000)
} // namespace

static int g_gic_log = 0;
uint64_t GicV3::read(uint64_t offset, unsigned size) {
    uint64_t abs = base() + offset;
    if (abs >= dist_base_ && abs < dist_base_ + dist_size_) {
        uint64_t v = dist_read(abs - dist_base_, size);
        if (g_gic_log++ < 0) HW_WARN("gic", "R dist+{:#x} = {:#x}", abs - dist_base_, v);
        return v;
    }
    if (abs >= redist_base_ && abs < redist_base_ + redist_size_) {
        uint64_t v = redist_read(abs - redist_base_, size);
        if (g_gic_log++ < 0) HW_WARN("gic", "R redist+{:#x} = {:#x}", abs - redist_base_, v);
        return v;
    }
    return 0;
}
void GicV3::write(uint64_t offset, uint64_t value, unsigned size) {
    uint64_t abs = base() + offset;
    if (abs >= dist_base_ && abs < dist_base_ + dist_size_) {
        if (g_gic_log++ < 0) HW_WARN("gic", "W dist+{:#x} = {:#x}", abs - dist_base_, value);
        dist_write(abs - dist_base_, value, size); return;
    }
    if (abs >= redist_base_ && abs < redist_base_ + redist_size_) {
        if (g_gic_log++ < 0) HW_WARN("gic", "W redist+{:#x} = {:#x}", abs - redist_base_, value);
        redist_write(abs - redist_base_, value, size); return;
    }
}

uint64_t GicV3::dist_read(uint64_t off, unsigned) {
    switch (off) {
        case GICD_CTLR:  return gicd_ctlr_ & ~(1u << 31);   // RWP=0: no write pending
        case GICD_TYPER: return 0x0000001F;                 // ITLinesNumber=31 -> 1020 SPIs
        case GICD_IIDR:  return 0x0000043B;                 // ARM implementer
        case GICD_PIDR2: return 0x30;                       // ArchRev=3 (GICv3)
        default:         return 0;                          // IGROUP/IPRIORITY/ICFG/... read 0
    }
}
void GicV3::dist_write(uint64_t off, uint64_t value, unsigned) {
    if (off == GICD_CTLR) gicd_ctlr_ = (uint32_t)value;
    // All other distributor writes (enables, priorities, configs) are accepted
    // and dropped -- we don't deliver interrupts yet.
}

uint64_t GicV3::redist_read(uint64_t off, unsigned size) {
    uint64_t frame = off / kRdFrameStride;
    uint64_t reg = off % kRdFrameStride;
    if (reg >= 0x10000) return 0;                           // SGI frame: config regs read 0

    switch (reg) {
        case GICR_CTLR:  return 0;
        case GICR_IIDR:  return 0x0000043B;
        case GICR_TYPER: {                                  // affinity in [63:32], Last=bit4
            uint64_t typer = (frame << 32) | (frame << 8) | (1u << 4); // Last set -> stop here
            if (size == 4) return (uint32_t)typer;          // low word (Last, procnum)
            return typer;
        }
        case GICR_TYPER + 4: return (uint32_t)(((frame << 32) | (frame << 8) | (1u << 4)) >> 32);
        case GICR_WAKER: {
            uint32_t ps = gicr_waker_ & 0x2;                // ChildrenAsleep tracks ProcessorSleep
            return (gicr_waker_ & ~0x4u) | (ps ? 0x4u : 0u);
        }
        case GICR_PIDR2: return 0x30;                       // ArchRev=3
        default:         return 0;
    }
}
void GicV3::redist_write(uint64_t off, uint64_t value, unsigned) {
    uint64_t reg = off % kRdFrameStride;
    if (reg == GICR_WAKER) gicr_waker_ = (uint32_t)value;   // driver clears ProcessorSleep
}

} // namespace hw::dev
