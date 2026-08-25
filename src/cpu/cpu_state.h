// ARM64 (AArch64) architectural state and the Linux boot contract.
//
// Per Documentation/arm64/booting.rst the primary CPU enters the kernel at the
// start of the loaded Image with the MMU off and:
//   x0 = physical address of the DTB, x1=x2=x3=0, PC = Image base.
// A concrete execution backend (Unicorn/QEMU-TCG) consumes this initial state.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hw::mem { class GuestMemory; }
namespace hw::dev { class DeviceBus; }

namespace hw::cpu {

struct Aarch64Regs {
    uint64_t x[31] = {};   // x0..x30
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint64_t pstate = 0;
};

class CpuState {
public:
    Aarch64Regs regs;
    // Configure the register file for the ARM64 Linux boot handoff.
    void setup_linux_boot(uint64_t kernel_entry, uint64_t dtb_addr);
};

// Reason a run stopped -- the raw material for actionable boot diagnostics.
struct RunResult {
    enum class Kind { Halted, InsnLimit, Exception, MemFault, Spin, Unsupported, NoBackend } kind;
    uint64_t pc = 0;
    uint64_t fault_addr = 0;
    bool     fault_is_write = false;
    unsigned fault_size = 0;
    std::string detail;
    uint64_t instructions_executed = 0;

    // Snapshot of the last device MMIO access before the stop (if any).
    bool     last_mmio_valid = false;
    bool     last_mmio_write = false;
    uint64_t last_mmio_addr = 0;
    uint64_t last_mmio_value = 0;
    uint64_t last_mmio_pc = 0;

    bool ok() const { return kind == Kind::Halted || kind == Kind::InsnLimit; }
};

// AArch64 MMU-related system registers exposed for diagnostics.
struct MmuRegs {
    bool valid = false;
    uint64_t ttbr0 = 0, ttbr1 = 0, mair = 0, vbar = 0, esr = 0, far_el1 = 0;
};

// Abstract execution engine. Implemented by a real ARM64 core (Unicorn).
class CpuBackend {
public:
    virtual ~CpuBackend() = default;
    virtual const char* name() const = 0;
    virtual bool attach(mem::GuestMemory& ram, dev::DeviceBus& bus, std::string& err) = 0;
    virtual void set_state(const CpuState& st) = 0;
    virtual RunResult run(uint64_t max_instructions) = 0;
    virtual Aarch64Regs read_regs() = 0;
    // Read guest memory honoring the current MMU translation (for diagnostics,
    // e.g. dumping the instructions around a faulting PC). Returns false on miss.
    virtual bool read_mem(uint64_t addr, void* buf, size_t len) = 0;
    // Disassemble the instruction at `pc` ("mnemonic operands"), or "" if the
    // backend has no disassembler. Default: none.
    virtual std::string disasm_at(uint64_t /*pc*/) { return {}; }
    // AArch64 MMU system registers (for page-table-walk diagnostics).
    virtual MmuRegs read_mmu_regs() { return {}; }
    // Recently executed guest PCs, oldest first (crash forensics).
    virtual std::vector<uint64_t> recent_pcs() const { return {}; }
};

} // namespace hw::cpu
