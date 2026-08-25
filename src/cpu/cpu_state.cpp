#include "cpu/cpu_state.h"

namespace hw::cpu {

void CpuState::setup_linux_boot(uint64_t kernel_entry, uint64_t dtb_addr) {
    regs = Aarch64Regs{};
    regs.x[0] = dtb_addr;   // x0 = DTB physical address
    regs.x[1] = 0;
    regs.x[2] = 0;
    regs.x[3] = 0;
    regs.pc = kernel_entry;
    // EL1h, DAIF masked (D,A,I,F set), as a bootloader hands off. (EL2 entry was
    // tried and behaves identically under Unicorn; EL1h is its best-supported mode.)
    regs.pstate = 0x3c5;
}

} // namespace hw::cpu
