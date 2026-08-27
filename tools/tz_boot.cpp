// Phase 11 Stage 1: standalone real-tz (Qualcomm TrustZone / QTEE) bring-up probe.
//
// Loads the REAL, unmodified `tz` ELF (extracted from the Quest 2 OTA) at its
// secure physical addresses, starts the CPU at EL3, and traces how far tz's cold
// boot runs before it touches something we have not provided. This EXECUTES the
// vendor's signed secure-monitor binary -- it does not fake a secure world.
//
// Requires unicorn built with the HOLLYWOOD_EL3 gate (cpu.c): this tool sets the
// env var itself before uc_open so has_el3 stays enabled.
//
// Build:
//   cl /nologo /MD /std:c++17 /EHsc /I third_party\unicorn-2.1.4\include \
//      tools\tz_boot.cpp /Fe:build\tz_boot.exe /Fo:build\ \
//      /link third_party\unicorn-build\Release\unicorn.lib ws2_32.lib
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

struct Seg { uint64_t type, flags, off, vaddr, paddr, filesz, memsz; };

static std::vector<uint8_t> g_tz;
static std::vector<Seg> g_segs;

// ---- trace state ----
static uint64_t g_insns = 0;
static uint64_t g_entry = 0;
static const uint64_t TRACE_FIRST = 400;      // print the first N instructions
static const int RING = 1024;
static uint64_t g_ring[RING]; static int g_ring_n = 0;   // last RING PCs
static bool g_faulted = false;
static uint64_t g_last_pc = ~0ull;
static bool g_spun = false;
static uint64_t g_trap_pc = 0;   // HOLLYWOOD_TZ_TRAP: dump ring + stop on first hit

static uint32_t insn_at(uc_engine* uc, uint64_t pc);

static void dump_ring(uc_engine* uc, const char* why, uint64_t pc) {
    std::printf("\n*** %s at %#llx after %llu insns -- preceding %d-insn trace:\n",
                why, (unsigned long long)pc, (unsigned long long)g_insns, RING);
    int cnt = (g_ring_n < RING) ? g_ring_n : RING;
    for (int i = 0; i < cnt; i++) {
        uint64_t ppc = g_ring[(g_ring_n - cnt + i) & (RING - 1)];
        std::printf("    %#011llx: %08x\n", (unsigned long long)ppc, insn_at(uc, ppc));
    }
}

template <class T> static T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }

static uint32_t insn_at(uc_engine* uc, uint64_t pc) {
    uint32_t w = 0; uc_mem_read(uc, pc, &w, 4); return w;
}

static void code_cb(uc_engine* uc, uint64_t pc, uint32_t size, void*) {
    g_insns++;
    if (g_insns <= TRACE_FIRST) {
        uint32_t w = insn_at(uc, pc);
        std::printf("  [%5llu] %#011llx: %08x\n", (unsigned long long)g_insns,
                    (unsigned long long)pc, w);
    }
    // Trap PC (HOLLYWOOD_TZ_TRAP): dump the preceding trace on first hit + stop.
    if (g_trap_pc && pc == g_trap_pc && !g_spun) {
        g_spun = true;
        dump_ring(uc, "TRAP", pc);
        uc_emu_stop(uc);
        return;
    }
    // Detect the park/error handler: a `b .` self-branch (opcode 0x14000000).
    if (insn_at(uc, pc) == 0x14000000u && !g_spun) {
        g_spun = true;
        dump_ring(uc, "SPIN (b .)", pc);
        uc_emu_stop(uc);
        return;
    }
    g_ring[g_ring_n++ & (RING - 1)] = pc;
    g_last_pc = pc;
}

static const char* memtype(uc_mem_type t) {
    switch (t) {
        case UC_MEM_READ_UNMAPPED:  return "READ  unmapped";
        case UC_MEM_WRITE_UNMAPPED: return "WRITE unmapped";
        case UC_MEM_FETCH_UNMAPPED: return "FETCH unmapped";
        case UC_MEM_READ_PROT:      return "READ  prot";
        case UC_MEM_WRITE_PROT:     return "WRITE prot";
        case UC_MEM_FETCH_PROT:     return "FETCH prot";
        default: return "mem?";
    }
}

static int g_automaps = 0;
static const int AUTOMAP_CAP = 400;
static std::vector<uint64_t> g_touched;   // distinct external pages, in order

static bool mem_cb(uc_engine* uc, uc_mem_type type, uint64_t addr, int size,
                   int64_t value, void*) {
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    uint64_t page = addr & ~0xfffull;

    // Execution into unmapped memory is structural (e.g. an ERET/handoff to a
    // world we haven't set up) -- log and stop.
    if (type == UC_MEM_FETCH_UNMAPPED || type == UC_MEM_FETCH_PROT) {
        std::printf("\n*** %s @ %#llx  (PC=%#llx, insn #%llu) -- structural, stopping\n",
                    memtype(type), (unsigned long long)addr,
                    (unsigned long long)pc, (unsigned long long)g_insns);
        g_faulted = true;
        return false;
    }
    // Permission faults on already-mapped RAM: log and stop (unexpected here).
    if (type == UC_MEM_READ_PROT || type == UC_MEM_WRITE_PROT) {
        std::printf("\n*** %s @ %#llx size=%d  (PC=%#llx, insn #%llu)\n",
                    memtype(type), (unsigned long long)addr, size,
                    (unsigned long long)pc, (unsigned long long)g_insns);
        g_faulted = true;
        return false;
    }
    // Data read/write to an unmapped external page: back it with a zero page,
    // log the (first) access, and continue -- this maps tz's dependency surface.
    if (g_automaps >= AUTOMAP_CAP) {
        std::printf("\n*** automap cap (%d) hit at %#llx (PC=%#llx) -- stopping\n",
                    AUTOMAP_CAP, (unsigned long long)addr, (unsigned long long)pc);
        g_faulted = true;
        return false;
    }
    uc_err me = uc_mem_map(uc, page, 0x1000, UC_PROT_ALL);
    if (me != UC_ERR_OK) {
        std::printf("\n*** map %#llx failed: %s (PC=%#llx) -- stopping\n",
                    (unsigned long long)page, uc_strerror(me), (unsigned long long)pc);
        g_faulted = true;
        return false;
    }
    g_automaps++;
    g_touched.push_back(page);
    std::printf("  [automap %3d] %-14s @ %#011llx (page %#011llx) sz=%d PC=%#011llx insn#%llu\n",
                g_automaps, memtype(type), (unsigned long long)addr,
                (unsigned long long)page, size, (unsigned long long)pc,
                (unsigned long long)g_insns);
    return true;   // retry the access now that the page is mapped
}

static void write_cb(uc_engine* uc, uc_mem_type type, uint64_t addr, int size,
                      int64_t value, void*) {
    if (addr >= 0x1468d000 && addr < 0x1468d020) {
        uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
        std::printf("  [WRITE] @%#llx = %#llx (size %d) PC=%#llx insn#%llu\n",
                    (unsigned long long)addr, (unsigned long long)value, size,
                    (unsigned long long)pc, (unsigned long long)g_insns);
    }
}

static void intr_cb(uc_engine* uc, uint32_t intno, void*) {
    uint64_t pc = 0, esr = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    // ESR_EL3 isn't exposed by uc; report intno + PC. intno encodes the EC-ish cause.
    std::printf("\n*** CPU EXCEPTION/INTR intno=%u  (PC=%#llx, insn #%llu)\n",
                intno, (unsigned long long)pc, (unsigned long long)g_insns);
}

static int esr_ec_hint(uc_engine* uc);

static void dump_regs(uc_engine* uc) {
    uint64_t x[31], sp, pc, ps;
    for (int i = 0; i < 31; i++) uc_reg_read(uc, UC_ARM64_REG_X0 + i, &x[i]);
    uc_reg_read(uc, UC_ARM64_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM64_REG_PSTATE, &ps);
    std::printf("\n--- registers ---\n");
    std::printf("PC=%#018llx  SP=%#018llx  PSTATE=%#llx (EL%llu%c)\n",
                (unsigned long long)pc, (unsigned long long)sp,
                (unsigned long long)ps, (unsigned long long)((ps >> 2) & 3),
                (ps & 1) ? 'h' : 't');
    for (int i = 0; i < 31; i += 2) {
        if (i + 1 < 31)
            std::printf("X%-2d=%#018llx  X%-2d=%#018llx\n", i, (unsigned long long)x[i],
                        i + 1, (unsigned long long)x[i + 1]);
        else
            std::printf("X%-2d=%#018llx\n", i, (unsigned long long)x[i]);
    }
    // EL3 exception context (via CP_REG): ELR/ESR/SPSR/FAR/VBAR_EL3.
    struct { const char* nm; uint8_t op0,op1,crn,crm,op2; } el3[] = {
        {"ELR_EL3",  3,6,4,0,1}, {"ESR_EL3",  3,6,5,2,0}, {"SPSR_EL3", 3,6,4,0,0},
        {"FAR_EL3",  3,6,6,0,0}, {"VBAR_EL3", 3,6,12,0,0}, {"SCR_EL3", 3,6,1,1,0},
        {"SCTLR_EL3",3,6,1,0,0},
    };
    std::printf("--- EL3 sysregs ---\n");
    for (auto& r : el3) {
        uc_arm64_cp_reg cp; std::memset(&cp, 0, sizeof cp);
        cp.op0 = r.op0; cp.op1 = r.op1; cp.crn = r.crn; cp.crm = r.crm; cp.op2 = r.op2;
        uc_err rr = uc_reg_read(uc, UC_ARM64_REG_CP_REG, &cp);
        std::printf("%-9s = %#018llx%s\n", r.nm, (unsigned long long)cp.val,
                    rr == UC_ERR_OK ? "" : " (read err)");
    }
    if (esr_ec_hint(uc)) {}
}

// Decode ESR_EL3.EC into a short hint. Returns 0 (only prints).
static int esr_ec_hint(uc_engine* uc) {
    uc_arm64_cp_reg cp; std::memset(&cp, 0, sizeof cp);
    cp.op0 = 3; cp.op1 = 6; cp.crn = 5; cp.crm = 2; cp.op2 = 0;   // ESR_EL3
    if (uc_reg_read(uc, UC_ARM64_REG_CP_REG, &cp) != UC_ERR_OK) return 0;
    unsigned ec = (unsigned)((cp.val >> 26) & 0x3f);
    const char* w = "?";
    switch (ec) {
        case 0x00: w = "unknown/undef (unsupported insn)"; break;
        case 0x07: w = "SVE/SIMD/FP trap"; break;
        case 0x18: w = "trapped MSR/MRS/sys insn"; break;
        case 0x16: w = "HVC"; break;
        case 0x17: w = "SMC"; break;
        case 0x20: case 0x21: w = "instruction abort"; break;
        case 0x24: case 0x25: w = "data abort"; break;
        case 0x22: w = "PC alignment"; break;
        case 0x26: w = "SP alignment"; break;
    }
    std::printf("ESR_EL3.EC = %#04x (%s)  ISS=%#llx\n", ec, w,
                (unsigned long long)(cp.val & 0x1ffffff));
    return 0;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1]
        : "C:\\Users\\drych\\.claude\\jobs\\c053367b\\tmp\\tz.img";
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    g_tz.resize(n); if (std::fread(g_tz.data(), 1, n, f) != (size_t)n) { std::printf("short read\n"); return 2; }
    std::fclose(f);
    std::printf("tz.img: %ld bytes\n", n);

    // ELF64 header
    if (std::memcmp(g_tz.data(), "\x7f""ELF", 4) != 0) { std::printf("not ELF\n"); return 2; }
    uint64_t e_entry = rd<uint64_t>(&g_tz[24]);
    uint64_t e_phoff = rd<uint64_t>(&g_tz[32]);
    uint16_t e_phentsize = rd<uint16_t>(&g_tz[54]);
    uint16_t e_phnum = rd<uint16_t>(&g_tz[56]);
    g_entry = e_entry;
    std::printf("e_entry=%#llx  phnum=%u\n", (unsigned long long)e_entry, e_phnum);

    for (int i = 0; i < e_phnum; i++) {
        const uint8_t* p = &g_tz[e_phoff + (uint64_t)i * e_phentsize];
        Seg s;
        s.type   = rd<uint32_t>(p + 0);
        s.flags  = rd<uint32_t>(p + 4);
        s.off    = rd<uint64_t>(p + 8);
        s.vaddr  = rd<uint64_t>(p + 16);
        s.paddr  = rd<uint64_t>(p + 24);
        s.filesz = rd<uint64_t>(p + 32);
        s.memsz  = rd<uint64_t>(p + 40);
        if (s.type == 1 /*PT_LOAD*/ && s.memsz > 0)
            g_segs.push_back(s);
    }
    std::printf("loadable segments: %zu\n", g_segs.size());

    uc_engine* uc = nullptr;
    _putenv("HOLLYWOOD_EL3=1");
    uc_err e = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc);
    if (e != UC_ERR_OK) { std::printf("uc_open: %s\n", uc_strerror(e)); return 2; }
    uc_err ce = uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX);
    if (ce != UC_ERR_OK) std::printf("set_cpu_model(max): %s\n", uc_strerror(ce));
    std::printf("HOLLYWOOD_EL3 env = %s\n", getenv("HOLLYWOOD_EL3") ? getenv("HOLLYWOOD_EL3") : "(unset)");
    { uint64_t ps0 = 0; uc_reg_read(uc, UC_ARM64_REG_PSTATE, &ps0);
      std::printf("PSTATE after reset (pre-write) = %#llx (EL%llu)\n",
                  (unsigned long long)ps0, (unsigned long long)((ps0 >> 2) & 3)); }
    // Probe whether SCR_EL3 is reachable (only if has_el3): read it via CP_REG.
    { uc_arm64_cp_reg cp; std::memset(&cp, 0, sizeof cp);
      cp.op0 = 3; cp.op1 = 6; cp.crn = 1; cp.crm = 1; cp.op2 = 0;   // SCR_EL3
      uc_err rr = uc_reg_read(uc, UC_ARM64_REG_CP_REG, &cp);
      std::printf("SCR_EL3 read via CP_REG: %s (val=%#llx)\n", uc_strerror(rr),
                  (unsigned long long)cp.val); }

    // Merge segment page-ranges into non-overlapping mappable regions.
    struct Rgn { uint64_t lo, hi; };
    std::vector<Rgn> rgns;
    {
        std::vector<Rgn> raw;
        for (auto& s : g_segs)
            raw.push_back({ s.paddr & ~0xfffull, (s.paddr + s.memsz + 0xfff) & ~0xfffull });
        std::sort(raw.begin(), raw.end(), [](const Rgn& a, const Rgn& b){ return a.lo < b.lo; });
        for (auto& r : raw) {
            if (!rgns.empty() && r.lo <= rgns.back().hi) { if (r.hi > rgns.back().hi) rgns.back().hi = r.hi; }
            else rgns.push_back(r);
        }
    }
    for (auto& r : rgns) {
        uc_err me = uc_mem_map(uc, r.lo, (size_t)(r.hi - r.lo), UC_PROT_ALL);
        std::printf("map secure region %#llx-%#llx (%llu KB): %s\n",
                    (unsigned long long)r.lo, (unsigned long long)r.hi,
                    (unsigned long long)((r.hi - r.lo) / 1024), uc_strerror(me));
        if (me != UC_ERR_OK) return 2;
    }
    for (auto& s : g_segs) {
        uc_err we = uc_mem_write(uc, s.paddr, g_tz.data() + s.off, (size_t)s.filesz);
        if (we != UC_ERR_OK)
            std::printf("write seg @%#llx filesz=%#llx: %s\n",
                        (unsigned long long)s.paddr, (unsigned long long)s.filesz, uc_strerror(we));
    }

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)code_cb, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                        UC_HOOK_MEM_FETCH_UNMAPPED | UC_HOOK_MEM_PROT,
                (void*)mem_cb, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_cb, nullptr, 1, 0);
    { uc_hook wh; uc_hook_add(uc, &wh, UC_HOOK_MEM_WRITE, (void*)write_cb, nullptr,
                              0x1468d000, 0x1468d01f); }

    // DISCOVERY PROBE: tz stashes entry x0/x1 (the xbl boot-handoff params) and
    // parks unless both are nonzero. Give nonzero scratch pointers and watch how tz
    // dereferences them to reconstruct the boot-info structure it expects.
    if (getenv("HOLLYWOOD_TZ_BOOTPARAMS")) {
        uc_mem_map(uc, 0x14700000, 0x20000, UC_PROT_ALL);   // scratch for boot structs
        uint64_t x0v = 0x14700000, x1v = 0x14710000;
        uc_reg_write(uc, UC_ARM64_REG_X0, &x0v);
        uc_reg_write(uc, UC_ARM64_REG_X1, &x1v);
        std::printf("BOOTPARAMS: x0=%#llx x1=%#llx (scratch mapped 0x14700000+0x20000)\n",
                    (unsigned long long)x0v, (unsigned long long)x1v);
    }

    // Start at EL3h with interrupts masked. x0..x3 = 0 (unknown cold-boot params).
    uint64_t pstate = 0x3cd;   // DAIF=1111, M=0b01101 = EL3h
    uc_reg_write(uc, UC_ARM64_REG_PSTATE, &pstate);
    { uint64_t ps1 = 0; uc_reg_read(uc, UC_ARM64_REG_PSTATE, &ps1);
      std::printf("PSTATE after writing 0x3cd = %#llx (EL%llu)\n",
                  (unsigned long long)ps1, (unsigned long long)((ps1 >> 2) & 3)); }
    uint64_t pc = e_entry;
    uc_reg_write(uc, UC_ARM64_REG_PC, &pc);

    if (const char* t = getenv("HOLLYWOOD_TZ_TRAP")) {
        g_trap_pc = std::strtoull(t, nullptr, 0);
        std::printf("trap PC = %#llx\n", (unsigned long long)g_trap_pc);
    }
    // DISCOVERY PROBE (provisional, not a fix): poke tz's config globals nonzero to
    // get past the "no valid boot config" park, to see how far tz then runs.
    if (getenv("HOLLYWOOD_TZ_POKEGLOBALS")) {
        uint32_t one = 1;
        uc_mem_write(uc, 0x1468d008, &one, 4);
        uc_mem_write(uc, 0x1468d010, &one, 4);
        std::printf("POKE: set globals 0x1468d008=1 0x1468d010=1\n");
    }

    std::printf("\n=== starting tz at EL3, PC=%#llx ===\n", (unsigned long long)e_entry);
    const uint64_t CAP = 30000000;
    uc_err re = uc_emu_start(uc, e_entry, 0, 0, CAP);
    std::printf("\n=== emu stopped: %s  (%llu instructions, %d external pages auto-mapped) ===\n",
                uc_strerror(re), (unsigned long long)g_insns, g_automaps);
    dump_regs(uc);

    std::printf("\n--- external pages tz touched (in order) ---\n");
    for (size_t i = 0; i < g_touched.size(); i++)
        std::printf("  %#011llx\n", (unsigned long long)g_touched[i]);

    // Show the last few PCs leading to the stop.
    std::printf("\n--- last PCs (oldest first) ---\n");
    int cnt = (g_insns < 32) ? (int)g_insns : 32;
    for (int i = 0; i < cnt; i++) {
        uint64_t ppc = g_ring[(g_ring_n - cnt + i) & 31];
        std::printf("  %#011llx: %08x\n", (unsigned long long)ppc, insn_at(uc, ppc));
    }
    return 0;
}
