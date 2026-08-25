// Qualcomm SM8250 "Kona" (Snapdragon XR2) platform constants.
//
// These are documented fallbacks; the emulator prefers values decoded from the
// device's own DTB at runtime. Addresses come from the kernel's Kona DTS
// (arch/arm64/boot/dts) and qcom platform headers.
#pragma once
#include <cstdint>

namespace hw::platform {

struct KonaMap {
    static constexpr uint64_t kDramBase       = 0x80000000ull;

    // ARM GICv3.
    static constexpr uint64_t kGicDistBase    = 0x17a00000ull;
    static constexpr uint64_t kGicDistSize    = 0x10000ull;
    static constexpr uint64_t kGicRedistBase  = 0x17a60000ull;
    static constexpr uint64_t kGicRedistSize  = 0x100000ull;

    // Memory-mapped system timer (CPU normally uses the CP15 arch timer).
    static constexpr uint64_t kArchTimerBase  = 0x17c20000ull;

    // GENI/QUP serial-engine consoles (typical debug UART on QUPV3_1).
    static constexpr uint64_t kQupUart0Base   = 0x00a90000ull;
    static constexpr uint64_t kSeSize         = 0x4000ull;

    static constexpr const char* kName        = "Qualcomm SM8250 Kona (Snapdragon XR2)";
    static constexpr unsigned    kNumCpus     = 8;
};

} // namespace hw::platform
