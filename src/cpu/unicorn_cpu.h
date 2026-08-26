// ARM64 execution backend built on the Unicorn Engine (QEMU TCG core).
//
// Design contract with the rest of hollywood_emu:
//   * Guest RAM is the emulator's single source of truth. We map the existing
//     GuestMemory buffer into Unicorn with uc_mem_map_ptr -- NOT a copy -- so
//     guest and host observe the same bytes.
//   * Every device on the DeviceBus is exposed to the guest via uc_mmio_map, so
//     MMIO reads/writes are routed to the real device model, with the faulting
//     PC available for diagnostics.
//   * Accesses that hit neither RAM nor a device are trapped, logged in detail,
//     and (by default) stop execution -- surfacing the next real blocker.
#pragma once
#include "cpu/cpu_state.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct uc_struct;      // uc_engine (opaque)
typedef struct uc_struct uc_engine;

namespace hw::dev { class MmioDevice; }

namespace hw::cpu {

struct UnicornOptions {
    bool trace = false;             // per-instruction PC (+ disasm) trace
    bool trace_disasm = true;       // disassemble in trace mode (capstone)
    uint64_t trace_limit = 300;     // cap trace lines to stay readable
    bool stop_on_unmapped = true;   // halt on the first unclaimed access
    bool log_mmio = false;          // log every device MMIO access
    uint64_t timeout_us = 20000000; // wall-clock cap (us) so a spin can't hang us
    uint64_t hot_threshold = 12000000; // per-PC hits within a window that flags a spin-wait (0=off)
    uint64_t heartbeat = 20000000;  // print progress every N insns (0 = off)
    bool code_hook = true;          // per-instruction hook (count/trace/spin) -- disable to test TCG interaction
    bool host_backed_ram = true;    // map guest RAM via uc_mem_map_ptr; false = uc_mem_map + copy
    bool step = false;              // single-step (one-instruction TBs) -- probes TB-boundary MMU stalls
    bool our_mmu = true;            // provide translation via our own ARMv8 walker (UC_TLB_VIRTUAL + TLB_FILL)
    bool vector_exc = true;         // deliver CPU exceptions to the guest's VBAR_EL1 handler
    bool stop_on_undef = false;     // halt on the first undefined-instruction exception (debug)
    std::string fn_trace_ksyms;     // path to a ksyms.txt; if set, trace a fixed set of
                                    // timer/irq functions (entry args + return value)
    bool trace_user = false;        // dump the exec->EL0 handoff (regs + initial stack:
                                    // argc/argv/envp/auxv/TLS), then trace EL0 instructions
                                    // and syscalls (SVC) -- userspace ABI diagnostics
    uint64_t trace_user_insns = 20000; // cap on traced EL0 instructions
};

class UnicornCpu : public CpuBackend {
public:
    explicit UnicornCpu(UnicornOptions opts = {});
    ~UnicornCpu() override;

    const char* name() const override { return "Unicorn (ARM64 / QEMU-TCG)"; }
    bool attach(mem::GuestMemory& ram, dev::DeviceBus& bus, std::string& err) override;
    void set_state(const CpuState& st) override;
    RunResult run(uint64_t max_instructions) override;
    Aarch64Regs read_regs() override;
    bool read_mem(uint64_t addr, void* buf, size_t len) override;
    std::string disasm_at(uint64_t pc) override;
    MmuRegs read_mmu_regs() override;
    // The most recently executed guest PCs, oldest first (for crash forensics).
    std::vector<uint64_t> recent_pcs() const;

    // Diagnostics captured during the last run.
    struct LastAccess { bool valid=false; bool is_write=false; uint64_t addr=0, value=0; unsigned size=0; uint64_t pc=0; };
    const LastAccess& last_mmio() const { return last_mmio_; }
    const LastAccess& fault() const { return fault_; }

private:
    // Unicorn C callbacks (thunk to instance methods).
    static uint64_t mmio_read_cb(uc_engine*, uint64_t offset, unsigned size, void* user);
    static void mmio_write_cb(uc_engine*, uint64_t offset, unsigned size, uint64_t value, void* user);
    static void code_cb(uc_engine*, uint64_t address, uint32_t size, void* user);
    // Per-basic-block hook (fast path): instruction counting, timer poll, spin
    // detection and heartbeat -- avoids the per-instruction UC_HOOK_CODE cost.
    static void block_cb(uc_engine*, uint64_t address, uint32_t size, void* user);
    static bool unmapped_cb(uc_engine*, int type, uint64_t address, int size, int64_t value, void* user);
    static void intr_cb(uc_engine*, uint32_t intno, void* user);
    // TLB-fill hook: we perform ARMv8 stage-1 translation ourselves.
    static bool tlb_cb(uc_engine*, uint64_t vaddr, int type, void* result, void* user);
    // SYS-instruction hook: flush our virtual TLB on TLBI so the guest never
    // reads stale page-table data (e.g. via the fixmap).
    static uint32_t sys_cb(uc_engine*, int reg, const void* cp_reg, void* user);
    // MRS/MSR hooks: emulate the GICv3 CPU-interface system registers (ICC_*),
    // which Unicorn's CPU doesn't implement (no GICv3 cpuif) and would trap.
    static uint32_t mrs_cb(uc_engine*, int reg, void* cp_reg, void* user);
    static uint32_t msr_cb(uc_engine*, int reg, void* cp_reg, void* user);
    // Software ARMv8 stage-1 walk (VA39/4KB). Sets `paddr` and returns true on a
    // hit; identity-maps low addresses (pre-MMU / idmap); a kernel-VA miss returns
    // false so the guest takes a real translation fault instead of a bad access.
    bool translate(uint64_t vaddr, uint64_t& paddr);

    void disasm_line(uint64_t pc);
    std::string disasm_str(uint64_t pc);

    UnicornOptions opts_;
    uc_engine* uc_ = nullptr;
    mem::GuestMemory* ram_ = nullptr;
    dev::DeviceBus* bus_ = nullptr;
    void* csh_ = nullptr;           // capstone handle (opaque)

    // Per-device MMIO routing context (kept alive for the engine's lifetime).
    struct MmioCtx { UnicornCpu* self; dev::MmioDevice* dev; uint64_t base; };
    std::vector<std::unique_ptr<MmioCtx>> mmio_ctxs_;

    uint64_t insns_ = 0;
    uint64_t traced_ = 0;
    uint64_t last_hb_ = 0;    // last heartbeat bucket (insns_/heartbeat)
    uint64_t last_win_ = 0;   // last spin-window bucket (insns_/kSpinWindow)
    LastAccess last_mmio_;
    LastAccess fault_;

    // Spin-wait detector: a coarse per-PC histogram; if any bucket gets too hot
    // the guest is almost certainly busy-waiting on hardware state that never
    // changes (a delay loop, a poll of an unimplemented status bit, ...).
    std::vector<uint32_t> hot_;
    bool spin_ = false;
    uint64_t spin_pc_ = 0;

    // CPU-exception storm detection (e.g. an abort that can't vector because
    // VBAR isn't set yet, so it re-raises forever).
    uint64_t exc_last_pc_ = 0;
    uint32_t exc_last_no_ = 0;
    uint64_t exc_repeat_ = 0;
    bool exc_storm_ = false;
    uint64_t exc_storm_pc_ = 0;
    uint32_t exc_storm_no_ = 0;
    uint64_t exc_vectored_ = 0;      // count of exceptions delivered to VBAR
    uint64_t last_tlb_miss_ = 0;     // last VA a TLB fill rejected (FAR for aborts)
    bool mmu_on_ = false;            // set once a high (kernel) VA is translated -- i.e. the
                                     // MMU + page tables are up. After that, a low-VA miss is a
                                     // real user fault (enables EL0 demand paging), not idmap.

    // --trace-user state: dump EL0 entry once, then trace user instructions/syscalls.
    bool user_entered_ = false;      // dumped the exec->EL0 handoff yet?
    uint64_t user_traced_ = 0;       // EL0 instructions traced so far
    uint64_t user_svc_count_ = 0;    // syscalls seen
    void dump_el0_entry();           // decode & print initial userspace state + stack
    uint64_t warns_skipped_ = 0;     // WARN/BUG brk instructions we recovered past
    uint32_t xlat_perms_ = 7;         // stage-1 perms (UC_PROT_*) from the last translate()

    // Emulated GICv3 CPU-interface (ICC_*) register file, keyed by encoding.
    std::unordered_map<uint32_t, uint64_t> icc_;

    // Spin detector histogram window: reset counts every this many instructions
    // so a real spin (dominates a window) is caught but long finite loops aren't.
    static constexpr uint64_t kSpinWindow = 0x4000000; // 64M instructions

    // Ring of recently executed PCs (crash forensics).
    static constexpr size_t kPcRing = 48;
    uint64_t pc_ring_[kPcRing] = {};
    size_t pc_ring_pos_ = 0;

    // Symbol-based function tracing (timer/irq debug). fn_watch_ maps a watched
    // function-entry VA to its name; fn_retstk_ holds pending (return-addr, name)
    // so we can log each call's return value (x0) when it returns.
    void load_fn_trace();
    std::unordered_map<uint64_t, std::string> fn_watch_;
    std::vector<std::pair<uint64_t, std::string>> fn_retstk_;
    uint64_t fn_trace_lines_ = 0;
};

const char* arm_excp_name(uint32_t intno);

} // namespace hw::cpu
