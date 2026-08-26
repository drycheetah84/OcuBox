// Device -> GIC -> CPU interrupt delivery bridge.
//
// The GICv3 distributor model mirrors its GICD_ISENABLER/ICENABLER state into the
// CPU (so an enabled INTID can be delivered), and device models assert/deassert
// their SPI line by INTID. Both go through the CPU execution backend, which owns
// the engine and the actual ICC_IAR/EOIR delivery path. The backend installs the
// hooks once its engine exists; before then these are no-ops.
#pragma once
#include <cstdint>

namespace hw::dev {

// Drive a device interrupt line (level-triggered). intid is the GIC INTID
// (SPI n -> 32 + n).
void raise_irq(uint32_t intid, bool level);

// Mirror the guest's GICD enable bit for an INTID so delivery is gated correctly.
void set_irq_enabled(uint32_t intid, bool enabled);

// Installed by the CPU backend (UnicornCpu::attach). ctx is opaque to callers.
void install_irq_backend(void (*raise)(uint32_t, bool), void (*enable)(uint32_t, bool));

} // namespace hw::dev
