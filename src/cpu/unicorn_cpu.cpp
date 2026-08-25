#include "cpu/unicorn_cpu.h"
#include "common/log.h"
#include "devices/device.h"
#include "devices/device_bus.h"
#include "memory/guest_memory.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <unicorn/unicorn.h>
#include <capstone/capstone.h>

namespace hw::cpu {

namespace {
// x0..x30 register ids. X0..X28 are contiguous in Unicorn's enum; x29/x30
// are the frame pointer / link register.
int xreg(int i) {
    if (i <= 28) return UC_ARM64_REG_X0 + i;
    return (i == 29) ? UC_ARM64_REG_X29 : UC_ARM64_REG_X30;
}
constexpr uint32_t kHotBits = 16;
constexpr uint32_t kHotMask = (1u << kHotBits) - 1;
} // namespace

// QEMU/Unicorn ARM EXCP_* numbers passed to UC_HOOK_INTR.
const char* arm_excp_name(uint32_t intno) {
    switch (intno) {
        case 1:  return "UDEF (undefined instruction)";
        case 2:  return "SWI/SVC";
        case 3:  return "PREFETCH_ABORT (instruction abort)";
        case 4:  return "DATA_ABORT";
        case 5:  return "IRQ";
        case 6:  return "FIQ";
        case 7:  return "BKPT";
        case 11: return "HVC";
        case 13: return "SMC";
        default: return "exception";
    }
}

UnicornCpu::UnicornCpu(UnicornOptions opts) : opts_(opts) {
    hot_.assign(1u << kHotBits, 0);
}

UnicornCpu::~UnicornCpu() {
    if (csh_) { csh h = (csh)(uintptr_t)csh_; cs_close(&h); }
    if (uc_) uc_close(uc_);
}

bool UnicornCpu::attach(mem::GuestMemory& ram, dev::DeviceBus& bus, std::string& err) {
    ram_ = &ram;
    bus_ = &bus;

    uc_err e = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc_);
    if (e != UC_ERR_OK) { err = std::string("uc_open: ") + uc_strerror(e); return false; }

    // Select the most capable ARM64 core. The Quest kernel programs TCR_EL1 with
    // IPS=0b100 (44-bit PA / larger-address features); the default A57 model can
    // stall in stage-1 translation once the MMU is enabled. "max" supports the
    // full feature set QEMU-TCG implements.
    int model = UC_CPU_ARM64_MAX;
    uc_err ce = uc_ctl_set_cpu_model(uc_, model);
    if (ce != UC_ERR_OK)
        HW_WARN("cpu.uc", "uc_ctl_set_cpu_model(max) failed ({}) -- using default", (int)ce);
    else
        HW_INFO("cpu.uc", "CPU model = ARM64 'max'");

    if (opts_.host_backed_ram) {
        // Single source of truth: map the emulator's RAM buffer directly (no copy).
        e = uc_mem_map_ptr(uc_, ram.base(), (size_t)ram.size(), UC_PROT_ALL, ram.host_ptr(ram.base()));
        if (e != UC_ERR_OK) { err = std::string("uc_mem_map_ptr(RAM): ") + uc_strerror(e); return false; }
        HW_INFO("cpu.uc", "mapped RAM {:#x}+{:#x} (host-backed)", ram.base(), ram.size());
    } else {
        // Unicorn-owned RAM; copy the currently-loaded image in. (Diagnostic mode:
        // isolates suspected uc_mem_map_ptr page-table-walk issues.)
        e = uc_mem_map(uc_, ram.base(), (size_t)ram.size(), UC_PROT_ALL);
        if (e != UC_ERR_OK) { err = std::string("uc_mem_map(RAM): ") + uc_strerror(e); return false; }
        e = uc_mem_write(uc_, ram.base(), ram.host_ptr(ram.base()), (size_t)ram.size());
        if (e != UC_ERR_OK) { err = std::string("uc_mem_write(RAM): ") + uc_strerror(e); return false; }
        HW_INFO("cpu.uc", "mapped RAM {:#x}+{:#x} (unicorn-owned, copied)", ram.base(), ram.size());
    }

    // Route each device's MMIO window to the real device model.
    for (const auto& d : bus.devices()) {
        auto ctx = std::make_unique<MmioCtx>();
        ctx->self = this; ctx->dev = d.get(); ctx->base = d->base();
        e = uc_mmio_map(uc_, d->base(), (size_t)d->size(),
                        mmio_read_cb, ctx.get(), mmio_write_cb, ctx.get());
        if (e != UC_ERR_OK) {
            err = std::string("uc_mmio_map(") + d->name() + "): " + uc_strerror(e);
            return false;
        }
        HW_INFO("cpu.uc", "mapped MMIO {} {:#x}+{:#x}", d->name(), d->base(), d->size());
        mmio_ctxs_.push_back(std::move(ctx));
    }

    // Take over address translation. Unicorn's built-in AArch64 stage-1 walker
    // spuriously prefetch-aborts on valid mappings once the guest MMU is on; we
    // switch to the virtual-TLB mode and fill translations ourselves with a
    // correct ARMv8 page-table walk (see translate()).
    if (opts_.our_mmu) {
        uc_err te = uc_ctl_tlb_mode(uc_, UC_TLB_VIRTUAL);
        if (te != UC_ERR_OK) HW_WARN("cpu.uc", "uc_ctl_tlb_mode(VIRTUAL) failed ({})", (int)te);
        uc_hook th;
        uc_hook_add(uc_, &th, UC_HOOK_TLB_FILL, (void*)tlb_cb, this, 1, 0);
        // Flush our virtual TLB on TLBI (SYS instruction, CRn=8) so page-table
        // edits the guest makes (notably fixmap remaps) are never read stale.
        uc_hook sh;
        uc_hook_add(uc_, &sh, UC_HOOK_INSN, (void*)sys_cb, this, 1, 0, UC_ARM64_INS_SYS);
        // (GICv3 CPU-interface ICC_* registers are now provided natively by our
        // patched Unicorn cpreg table, so no MRS/MSR interception is needed.)
        HW_INFO("cpu.uc", "translation provided by built-in ARMv8 walker (UC_TLB_VIRTUAL)");
    }

    uc_hook h;
    if (opts_.code_hook) {
        // Fast path: a per-block hook carries instruction counting, the arch-timer
        // poll, spin detection and the heartbeat. The per-instruction UC_HOOK_CODE
        // (which forces TCG to instrument every instruction and is ~10-50x slower)
        // is only added when we actually need per-instruction visibility.
        uc_hook_add(uc_, &h, UC_HOOK_BLOCK, (void*)block_cb, this, 1, 0);
        const bool need_insn_hook = opts_.trace || !opts_.fn_trace_ksyms.empty();
        if (need_insn_hook)
            uc_hook_add(uc_, &h, UC_HOOK_CODE, (void*)code_cb, this, 1, 0);
        uc_hook_add(uc_, &h, UC_HOOK_MEM_UNMAPPED, (void*)unmapped_cb, this, 1, 0);
        uc_hook_add(uc_, &h, UC_HOOK_MEM_PROT, (void*)unmapped_cb, this, 1, 0);
        uc_hook_add(uc_, &h, UC_HOOK_INTR, (void*)intr_cb, this, 1, 0);
    }

    // Disassembler for trace mode (best-effort).
    csh handle = 0;
    cs_err cse = cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle);
    if (cse == CS_ERR_OK) csh_ = (void*)(uintptr_t)handle;
    else HW_WARN("cpu.uc", "capstone cs_open failed ({}): disasm disabled", (int)cse);

    if (!opts_.fn_trace_ksyms.empty()) load_fn_trace();

    return true;
}

// Load the addresses of a fixed set of timer/irq functions from a ksyms.txt
// ("<hexaddr> <type> <name>" per line) so code_cb can trace their calls.
void UnicornCpu::load_fn_trace() {
    static const char* kWatch[] = {
        "irq_of_parse_and_map", "irq_create_fwspec_mapping",
        "gic_irq_domain_translate", "gic_irq_domain_alloc", "gic_irq_domain_map",
        "irq_domain_alloc_descs", "__irq_alloc_descs", "irq_set_percpu_devid",
    };
    std::FILE* f = std::fopen(opts_.fn_trace_ksyms.c_str(), "rb");
    if (!f) { HW_WARN("trace", "fn-trace: cannot open {}", opts_.fn_trace_ksyms); return; }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long addr; char type; char nm[256];
        if (std::sscanf(line, "%llx %c %255s", &addr, &type, nm) != 3) continue;
        for (const char* w : kWatch) if (std::strcmp(w, nm) == 0) { fn_watch_[addr] = nm; break; }
    }
    std::fclose(f);
    HW_WARN("trace", "fn-trace: watching {} function entries", fn_watch_.size());
}

void UnicornCpu::set_state(const CpuState& st) {
    for (int i = 0; i < 31; ++i) { uint64_t v = st.regs.x[i]; uc_reg_write(uc_, xreg(i), &v); }
    uint64_t sp = st.regs.sp, pc = st.regs.pc, pstate = st.regs.pstate;
    uc_reg_write(uc_, UC_ARM64_REG_SP, &sp);
    uc_reg_write(uc_, UC_ARM64_REG_PC, &pc);
    uc_reg_write(uc_, UC_ARM64_REG_PSTATE, &pstate);
    HW_INFO("cpu.uc", "state set: PC={:#x} X0={:#x} PSTATE={:#x}", pc, st.regs.x[0], pstate);
}

RunResult UnicornCpu::run(uint64_t max_instructions) {
    insns_ = 0; traced_ = 0; fault_.valid = false; spin_ = false; spin_pc_ = 0;
    exc_last_pc_ = 0; exc_last_no_ = 0; exc_repeat_ = 0; exc_storm_ = false;
    exc_vectored_ = 0; last_tlb_miss_ = 0; warns_skipped_ = 0;
    std::memset(hot_.data(), 0, hot_.size() * sizeof(uint32_t));
    uint64_t pc = 0; uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);

    if (opts_.trace) HW_INFO("cpu.uc", "tracing first {} instructions", opts_.trace_limit);
    auto t0 = std::chrono::steady_clock::now();
    uc_err e = UC_ERR_OK;
    uint64_t elapsed_us = 0;
    if (opts_.step) {
        // Single-step: one instruction per uc_emu_start (one TB each). Slower, but
        // it changes how QEMU regenerates the TB straddling the MMU-enable.
        for (uint64_t i = 0; i < max_instructions; ++i) {
            uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);
            if (opts_.code_hook == false) insns_++;   // count here if no per-insn hook
            e = uc_emu_start(uc_, pc, 0, 0, 1);
            if (e != UC_ERR_OK || spin_) break;
            elapsed_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (opts_.timeout_us && elapsed_us >= opts_.timeout_us) break;
        }
    } else {
        // Run with WFI/idle handling: when the guest executes WFI it has finished
        // work and is waiting for an interrupt, so uc_emu_start returns cleanly.
        // Poll the (host-time-based) generic timer and resume, so timer ticks keep
        // flowing and the scheduler/idle loop makes progress. A genuine dead stall
        // (waiting on an IRQ we never deliver) is bounded by the wall-clock timeout.
        for (;;) {
            uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);
            uint64_t el = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (opts_.timeout_us && el >= opts_.timeout_us) break;
            if (insns_ >= max_instructions) break;
            uint64_t rem_to = opts_.timeout_us ? (opts_.timeout_us - el) : 0;
            uint64_t rem_ins = max_instructions - insns_;
            e = uc_emu_start(uc_, pc, 0, rem_to, rem_ins);
            if (e != UC_ERR_OK || spin_) break;              // real stop / error / spin
            if (insns_ >= max_instructions) break;           // hit the instruction cap
            el = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (opts_.timeout_us && el >= opts_.timeout_us) break;   // timed out
            // Clean return with budget left => WFI idle halt. Advance the timer so a
            // due tick raises the CPU IRQ line, then resume (WFI falls through once
            // the interrupt is pending). Brief sleep so we don't spin at 100% while
            // the host-time counter climbs to the programmed compare value.
            uc_arm64_timer_poll(uc_);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    elapsed_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();

    RunResult r;
    r.instructions_executed = insns_;
    uc_reg_read(uc_, UC_ARM64_REG_PC, &r.pc);
    if (last_mmio_.valid) {
        r.last_mmio_valid = true;
        r.last_mmio_write = last_mmio_.is_write;
        r.last_mmio_addr = last_mmio_.addr;
        r.last_mmio_value = last_mmio_.value;
        r.last_mmio_pc = last_mmio_.pc;
    }
    if (fault_.valid) { r.fault_is_write = fault_.is_write; r.fault_size = fault_.size; }

    bool timed_out = opts_.timeout_us && elapsed_us >= opts_.timeout_us && insns_ < max_instructions;

    if (exc_storm_ || (e == UC_ERR_EXCEPTION)) {
        r.kind = RunResult::Kind::Exception;
        uint32_t no = exc_storm_ ? exc_storm_no_ : 0;
        if (exc_storm_) {
            r.pc = exc_storm_pc_;
            r.detail = std::string("repeated CPU exception: ") + arm_excp_name(no) +
                       " re-raised at the same PC (cannot be delivered -- e.g. VBAR not set)";
        } else {
            r.detail = std::string("unhandled CPU exception (") + uc_strerror(e) + ")";
        }
    } else if (spin_) {
        r.kind = RunResult::Kind::Spin;
        r.pc = spin_pc_;
        r.fault_addr = spin_pc_;
        r.detail = "hot loop / spin-wait detected (guest is busy-waiting on hardware state)";
    } else if (e != UC_ERR_OK && fault_.valid) {
        r.kind = RunResult::Kind::MemFault;
        r.fault_addr = fault_.addr;
        r.detail = std::string(uc_strerror(e)) + " (unclaimed guest memory access)";
    } else if (e != UC_ERR_OK) {
        r.kind = RunResult::Kind::Exception;
        r.detail = uc_strerror(e);
    } else if (insns_ >= max_instructions) {
        r.kind = RunResult::Kind::InsnLimit;
        r.detail = "reached instruction limit";
    } else if (timed_out) {
        r.kind = RunResult::Kind::Spin;
        r.detail = "wall-clock timeout (no fault, no halt -- likely a slow spin-wait)";
    } else {
        r.kind = RunResult::Kind::Halted;
        r.detail = "execution halted";
    }
    return r;
}

Aarch64Regs UnicornCpu::read_regs() {
    Aarch64Regs regs;
    for (int i = 0; i < 31; ++i) uc_reg_read(uc_, xreg(i), &regs.x[i]);
    uc_reg_read(uc_, UC_ARM64_REG_SP, &regs.sp);
    uc_reg_read(uc_, UC_ARM64_REG_PC, &regs.pc);
    uc_reg_read(uc_, UC_ARM64_REG_PSTATE, &regs.pstate);
    return regs;
}

bool UnicornCpu::read_mem(uint64_t addr, void* buf, size_t len) {
    if (uc_ && uc_mem_read(uc_, addr, buf, len) == UC_ERR_OK) return true;
    // Fall back to our own translation (uc_mem_read works on physical addrs; the
    // guest PC may be a kernel virtual address once the MMU is on).
    if (ram_) {
        uint64_t pa = 0;
        if (translate(addr, pa) && ram_->contains(pa, len)) {
            std::memcpy(buf, ram_->host_ptr(pa), len);
            return true;
        }
    }
    return false;
}

// ---- Unicorn C callbacks ----

uint64_t UnicornCpu::mmio_read_cb(uc_engine* uc, uint64_t offset, unsigned size, void* user) {
    auto* ctx = static_cast<MmioCtx*>(user);
    UnicornCpu* self = ctx->self;
    uint64_t val = ctx->dev->read(offset, size);
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    self->last_mmio_ = { true, false, ctx->base + offset, val, size, pc };
    if (self->opts_.log_mmio)
        HW_INFO("mmio", "PC={:#x} READ  {:#x} size={} -> {:#x} [{}]",
                pc, ctx->base + offset, size, val, ctx->dev->name());
    return val;
}

void UnicornCpu::mmio_write_cb(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user) {
    auto* ctx = static_cast<MmioCtx*>(user);
    UnicornCpu* self = ctx->self;
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    self->last_mmio_ = { true, true, ctx->base + offset, value, size, pc };
    if (self->opts_.log_mmio)
        HW_INFO("mmio", "PC={:#x} WRITE {:#x} size={} value={:#x} [{}]",
                pc, ctx->base + offset, size, value, ctx->dev->name());
    ctx->dev->write(offset, value, size);
}

std::vector<uint64_t> UnicornCpu::recent_pcs() const {
    std::vector<uint64_t> out;
    for (size_t i = 0; i < kPcRing; ++i) {
        uint64_t v = pc_ring_[(pc_ring_pos_ + i) % kPcRing];
        if (v) out.push_back(v);
    }
    return out;
}

// Fast path: fires once per translated basic block (not per instruction), so
// TCG can run blocks natively. Carries instruction counting, the arch-timer
// poll (interrupts are checked at block boundaries anyway), spin detection and
// the heartbeat.
void UnicornCpu::block_cb(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    self->insns_ += size ? (size / 4) : 1;          // block size in bytes -> #insns (AArch64)
    self->pc_ring_[self->pc_ring_pos_] = address;
    self->pc_ring_pos_ = (self->pc_ring_pos_ + 1) % kPcRing;

    // Drive the ARM generic timer: recompute its output and the CPU IRQ line.
    uc_arm64_timer_poll(uc);

    // Spin-wait detection (windowed): a real spin dominates a short window of
    // execution; legitimate long loops complete and move on. Reset the histogram
    // per window so cumulative counts over a long boot don't false-positive.
    if (self->opts_.hot_threshold) {
        uint64_t win = self->insns_ / kSpinWindow;
        if (win != self->last_win_) {
            self->last_win_ = win;
            std::memset(self->hot_.data(), 0, self->hot_.size() * sizeof(uint32_t));
        }
        uint32_t& bucket = self->hot_[(uint32_t)(address >> 2) & kHotMask];
        if (++bucket >= (uint32_t)self->opts_.hot_threshold && !self->spin_) {
            self->spin_ = true;
            self->spin_pc_ = address;
            uc_emu_stop(uc);
            return;
        }
    }

    if (self->opts_.heartbeat) {
        uint64_t hb = self->insns_ / self->opts_.heartbeat;
        if (hb != self->last_hb_) {
            self->last_hb_ = hb;
            std::printf("  \x1b[2m[cpu]\x1b[0m executed %lluM insns, PC=%#llx\n",
                        (unsigned long long)(self->insns_ / 1000000), (unsigned long long)address);
            std::fflush(stdout);
        }
    }
}

// Per-instruction hook: only registered when tracing (--trace) or symbol
// function-tracing (--trace-irq) is active. Does NOT count instructions (the
// block hook owns the count) to avoid double-counting.
void UnicornCpu::code_cb(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);

    if (self->opts_.trace && self->traced_ < self->opts_.trace_limit) {
        self->traced_++;
        self->disasm_line(address);
    }

    // Symbol-based function tracing: log entry (args) and return value (x0).
    if (!self->fn_watch_.empty() && self->fn_trace_lines_ < 4000) {
        auto it = self->fn_watch_.find(address);
        if (it != self->fn_watch_.end()) {
            uint64_t x0=0,x1=0,x2=0,lr=0;
            uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
            uc_reg_read(uc, UC_ARM64_REG_X1, &x1);
            uc_reg_read(uc, UC_ARM64_REG_X2, &x2);
            uc_reg_read(uc, UC_ARM64_REG_LR, &lr);
            HW_WARN("trace", "{}> {}(x0={:#x} x1={:#x} x2={:#x}) lr={:#x}",
                    std::string(self->fn_retstk_.size()*2, ' '), it->second, x0, x1, x2, lr);
            self->fn_retstk_.push_back({lr, it->second});
            self->fn_trace_lines_++;
        }
        while (!self->fn_retstk_.empty() && address == self->fn_retstk_.back().first) {
            uint64_t x0=0; uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
            auto nm = self->fn_retstk_.back().second; self->fn_retstk_.pop_back();
            HW_WARN("trace", "{}< {} = {:#x}", std::string(self->fn_retstk_.size()*2, ' '), nm, x0);
            self->fn_trace_lines_++;
        }
    }
}

bool UnicornCpu::unmapped_cb(uc_engine* uc, int type, uint64_t address, int size, int64_t value, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    bool is_write = (type == UC_MEM_WRITE_UNMAPPED || type == UC_MEM_WRITE_PROT);
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    self->fault_ = { true, is_write, address, (uint64_t)value, (unsigned)size, pc };

    const char* kind = "UNMAPPED";
    if (type == UC_MEM_READ_PROT || type == UC_MEM_WRITE_PROT || type == UC_MEM_FETCH_PROT) kind = "PROT";
    else if (type == UC_MEM_FETCH_UNMAPPED) kind = "FETCH-UNMAPPED";
    HW_WARN("mmio", "PC={:#x} {} {} {:#x} size={}{}", pc, kind, is_write ? "WRITE" : "READ",
            address, size, is_write ? "" : "");

    if (self->opts_.stop_on_unmapped) return false;   // stop -> surface the blocker

    // Permissive mode: back the page with zero RAM and continue.
    uint64_t page = address & ~0xfffull;
    uc_mem_map(uc, page, 0x1000, UC_PROT_ALL);
    return true;
}

void UnicornCpu::intr_cb(uc_engine* uc, uint32_t intno, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    // The exception is now delivered to the guest's vector table natively (our
    // patched cpu_handle_exception calls do_interrupt, which sets SPSR/ELR/ESR/
    // FAR/PSTATE correctly). This hook is observe-only, plus a safety net: if the
    // SAME exception re-raises at the SAME PC an absurd number of times, a guest
    // handler is wedged -- stop rather than spin forever.
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    if (self->opts_.trace || self->opts_.log_mmio)
        HW_INFO("cpu.uc", "exception intno={} ({}) PC={:#x}", intno, arm_excp_name(intno), pc);

    // Debug: catch the first undefined-instruction at the moment it occurs, so
    // the recent-PC trace shows how control flow got there (before the oops).
    if (intno == 1 && self->opts_.stop_on_undef && !self->exc_storm_) {
        self->exc_storm_ = true; self->exc_storm_pc_ = pc; self->exc_storm_no_ = intno;
        uc_emu_stop(uc);
        return;
    }

    if (pc == self->exc_last_pc_ && intno == self->exc_last_no_) self->exc_repeat_++;
    else self->exc_repeat_ = 0;
    self->exc_last_pc_ = pc; self->exc_last_no_ = intno;
    if (self->exc_repeat_ > 4000000 && !self->exc_storm_) {
        self->exc_storm_ = true;
        self->exc_storm_pc_ = pc;
        self->exc_storm_no_ = intno;
        uc_emu_stop(uc);
    }
}

std::string UnicornCpu::disasm_str(uint64_t pc) {
    uint8_t code[4] = {};
    if (!read_mem(pc, code, 4)) return "<unreadable>";
    if (csh_) {
        cs_insn* insn = nullptr;
        size_t n = cs_disasm((csh)(uintptr_t)csh_, code, 4, pc, 1, &insn);
        if (n > 0) {
            std::string s = std::string(insn[0].mnemonic) + " " + insn[0].op_str;
            cs_free(insn, n);
            return s;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), ".word 0x%02x%02x%02x%02x", code[3], code[2], code[1], code[0]);
    return buf;
}

std::string UnicornCpu::disasm_at(uint64_t pc) { return disasm_str(pc); }

MmuRegs UnicornCpu::read_mmu_regs() {
    MmuRegs m; m.valid = true;
    uc_reg_read(uc_, UC_ARM64_REG_TTBR0_EL1, &m.ttbr0);
    uc_reg_read(uc_, UC_ARM64_REG_TTBR1_EL1, &m.ttbr1);
    uc_reg_read(uc_, UC_ARM64_REG_MAIR_EL1, &m.mair);
    uc_reg_read(uc_, UC_ARM64_REG_VBAR_EL1, &m.vbar);
    uc_reg_read(uc_, UC_ARM64_REG_ESR_EL1, &m.esr);
    uc_reg_read(uc_, UC_ARM64_REG_FAR_EL1, &m.far_el1);
    return m;
}

bool UnicornCpu::translate(uint64_t vaddr, uint64_t& paddr) {
    if (!ram_) { paddr = vaddr; return true; }
    uint64_t ttbr0 = 0, ttbr1 = 0;
    uc_reg_read(uc_, UC_ARM64_REG_TTBR0_EL1, &ttbr0);
    uc_reg_read(uc_, UC_ARM64_REG_TTBR1_EL1, &ttbr1);
    const bool high = (vaddr >> 63) & 1;                       // kernel-half VAs -> TTBR1
    if (high) mmu_on_ = true;   // high VAs only exist once the MMU + kernel page tables are up
    uint64_t table = (high ? ttbr1 : ttbr0) & 0x0000fffffffff000ull;

    if (table != 0) {
        const int shift[3] = { 30, 21, 12 };                  // L1/L2/L3 (VA39, 4KB)
        uint64_t t = table;
        for (int i = 0; i < 3; ++i) {
            uint64_t idx = (vaddr >> shift[i]) & 0x1ff;
            uint64_t da = t + idx * 8;
            if (!ram_->contains(da, 8)) break;
            uint64_t desc = ram_->read64(da);
            if ((desc & 1) == 0) break;                       // invalid -> miss
            uint64_t next = desc & 0x0000fffffffff000ull;
            if ((desc & 3) == 1) {                            // block
                uint64_t mask = (i == 0) ? 0x3fffffffull : 0x1fffffull;
                paddr = (next & ~mask) | (vaddr & mask); return true;
            }
            if (i == 2) { paddr = next | (vaddr & 0xfffull); return true; }  // L3 page
            t = next;
        }
    }
    // Linear-map (PAGE_OFFSET) fallback. The arm64 linear map is a fixed-offset
    // alias of all RAM: VA = PA - PHYS_OFFSET + PAGE_OFFSET (VA39: PAGE_OFFSET =
    // 0xffffffc000000000). The kernel writes patched code through lm_alias() of
    // .init.text during "alternatives: patching kernel code"; that page is not
    // present in the page-table walk (map_mem leaves the init image out of the
    // linear map), yet the guest expects the alias to reach the physical page.
    // Resolve it directly so self-patching lands on the real RAM bytes.
    constexpr uint64_t kLinearBase = 0xffffffc000000000ull;   // VA39 PAGE_OFFSET
    if (vaddr >= kLinearBase && vaddr < kLinearBase + ram_->size()) {
        paddr = ram_->base() + (vaddr - kLinearBase);
        return true;
    }
    // Kernel-half miss => real translation fault. Low-half miss: once the MMU is
    // on, an unmapped user VA is a real fault (so EL0 demand-paging works -- e.g.
    // /init's pages fault in on first access); only before the MMU comes up do we
    // identity-map low addresses (pre-MMU / idmap physical accesses).
    if (high) return false;
    if (mmu_on_) {
        static int lo_miss_log = 0;
        if (lo_miss_log < 200) { lo_miss_log++;
            uint64_t pc = 0; uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);
            HW_WARN("cpu.uc", "EL0/low fault VA={:#x} at PC={:#x}", vaddr, pc); }
        return false;
    }
    paddr = vaddr; return true;
}

uint32_t UnicornCpu::sys_cb(uc_engine* uc, int /*reg*/, const void* cp_reg, void* /*user*/) {
    const auto* cp = static_cast<const uc_arm64_cp_reg*>(cp_reg);
    if (cp && cp->crn == 8)          // TLBI (CRn=8): a translation just changed
        uc_ctl_flush_tlb(uc);
    return 0;                        // let the instruction execute normally
}

namespace {
// Is this system register a GICv3 CPU interface (ICC_*) register at EL1?
inline bool is_icc(const uc_arm64_cp_reg* cp) {
    return cp && cp->op0 == 3 && cp->op1 == 0 &&
           (cp->crn == 12 || (cp->crn == 4 && cp->crm == 6));
}
inline uint32_t icc_key(const uc_arm64_cp_reg* cp) {
    return (cp->crn << 8) | (cp->crm << 4) | cp->op2;
}
} // namespace

uint32_t UnicornCpu::mrs_cb(uc_engine* uc, int reg, void* cp_reg, void* user) {
    auto* cp = static_cast<uc_arm64_cp_reg*>(cp_reg);
    if (!is_icc(cp)) return 0;                        // not ICC: execute normally
    auto* self = static_cast<UnicornCpu*>(user);
    uint32_t key = icc_key(cp);
    uint64_t val = self->icc_.count(key) ? self->icc_[key] : 0;
    if (cp->crn == 12 && cp->crm == 12 && cp->op2 == 5) val |= 0x1;         // ICC_SRE_EL1.SRE
    if (cp->crn == 12 && cp->op2 == 0 && (cp->crm == 12 || cp->crm == 8)) val = 1023; // IAR: spurious
    if (cp->crn == 12 && cp->crm == 11 && cp->op2 == 3) val = 0xff;          // ICC_RPR_EL1 idle
    uc_reg_write(uc, reg, &val);
    return 1;                                         // handled: skip the real MRS
}

uint32_t UnicornCpu::msr_cb(uc_engine* uc, int reg, void* cp_reg, void* user) {
    auto* cp = static_cast<uc_arm64_cp_reg*>(cp_reg);
    if (!is_icc(cp)) return 0;                        // not ICC: execute normally
    auto* self = static_cast<UnicornCpu*>(user);
    uint64_t val = 0;
    uc_reg_read(uc, reg, &val);                       // value being written (source Xt)
    self->icc_[icc_key(cp)] = val;
    return 1;                                         // handled: skip the real MSR
}

bool UnicornCpu::tlb_cb(uc_engine*, uint64_t vaddr, int /*type*/, void* result, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    auto* e = static_cast<uc_tlb_entry*>(result);
    uint64_t pa = 0;
    if (!self->translate(vaddr, pa)) { self->last_tlb_miss_ = vaddr; return false; }  // fault
    e->paddr = pa;
    e->perms = UC_PROT_ALL;
    return true;
}

void UnicornCpu::disasm_line(uint64_t pc) {
    std::printf("  \x1b[2m[cpu]\x1b[0m %#010llx: %s\n", (unsigned long long)pc, disasm_str(pc).c_str());
}

} // namespace hw::cpu
