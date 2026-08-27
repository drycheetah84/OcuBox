// Faithful full-context reproducer: runs the REAL soinfo::relocate_relr machine
// code (vaddr 0x248d0, 90 bytes, self-contained) over the REAL .relr.dyn against
// the REAL GNU_RELRO data at the REAL load bias. A fake soinfo supplies the three
// fields relocate_relr reads: [0x11c]=load_bias, [0x1b8]=relr_ptr, [0x1bc]=count.
// Unlike the hand-loop, the real code relocates each set bit via an IT-block
// conditional store (str r5,[r3,r4] under `IT MI`) -- the path the hand-loop's
// explicit `bpl` branch never exercised.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <set>
#include <vector>

static const uint64_t BASE     = 0xf7982000ull;
static const uint64_t RELR_VA  = BASE + 0x710;
static const uint32_t RELR_SZ  = 0x2b0;                 // 0x2b0/4 = 172 entries
static const uint64_t RELRO_VA = BASE + 0xd1950;
static const uint32_t RELRO_FILESZ = 0x4cb4, RELRO_MEMSZ = 0x56b0;
static const uint64_t FUNC_VA  = BASE + 0x248d0;        // soinfo::relocate_relr
static const uint32_t FUNC_FOFF = 0x238d0, FUNC_SZ = 0x5a;
static const uint64_t SOINFO   = 0x10000;
static const uint64_t TABLE9 = BASE+0xd6088, TABLE10 = BASE+0xd608c, TABLE11 = BASE+0xd6090;

static bool g_cow = false;
static std::set<uint64_t> g_cowed;
static bool in_relro(uint64_t va){ return va >= (RELRO_VA & ~0xfffull) && va < ((RELRO_VA + RELRO_MEMSZ + 0xfff) & ~0xfffull); }

extern "C" void uc_arm64_exec_state(uc_engine*, uint64_t*);
static const uint64_t STORE_PC = FUNC_VA + (0x24914 - 0x248d0);   // bitmap str r5,[r3,r4]
static const uint64_t LOOP_LO  = FUNC_VA + (0x24908 - 0x248d0);   // loop head (lsls)
static const uint64_t LOOP_HI  = FUNC_VA + (0x24920 - 0x248d0);
static long g_seq = 0;
// Full per-instruction trace of the RELR bitmap loop while it is processing the
// .init_array page (r3+r4 in [page,page+0x1000)). Shows how r3 (where) advances and
// whether the 0x8c iteration's loop-head/store/adds ever execute.
static void code_cb(uc_engine* uc, uint64_t pc, uint32_t, void*) {
    uint64_t es[24] = {0}; uc_arm64_exec_state(uc, es);
    uint64_t r2 = es[5+2], r3 = es[5+3], r4 = es[5+4], r5 = es[5+5];
    uint64_t itstate = es[21], cpsr = es[22];
    if (r3 < 0xd6080 || r3 > 0xd6098) return;   // iterations near t9/t10/t11, gated by r3 (stable)
    uint64_t nzcv = 0; uc_reg_read(uc, UC_ARM64_REG_NZCV, &nzcv);
    const char* lbl =
        (pc == LOOP_LO)        ? "lsls" :
        (pc == FUNC_VA + 0x3a) ? "IT"   :
        (pc == FUNC_VA + 0x3c) ? "ldr.w r4=bias" :
        (pc == FUNC_VA + 0x40) ? "ldr r5" :
        (pc == FUNC_VA + 0x42) ? "add r5" :
        (pc == STORE_PC)       ? "STR" :
        (pc == STORE_PC + 2)   ? "adds r3" :
        (pc == FUNC_VA + 0x48) ? "lsrs" : "";
    std::printf("  [%04ld] pc=+%03x %-14s r2=%08x r3=%08x r5=%08x N=%d IT=%02x cpsr=%08x\n",
                g_seq++, (unsigned)(pc - FUNC_VA), lbl,
                (unsigned)r2, (unsigned)r3, (unsigned)r5,
                (int)((nzcv >> 31) & 1), (unsigned)itstate, (unsigned)cpsr);
}
static void nop_cb(uc_engine*, uint64_t, uint32_t, void*) {}   // forces single-instruction TBs
static bool g_noexec = false;   // EXPERIMENT: strip EXEC from the RELRO data page
static bool tlb_cb(uc_engine*, uint64_t va, int, void* result, void*) {
    struct Ent { uint64_t paddr; uint32_t perms; }* e = (Ent*)result;
    e->paddr = va;
    bool relro = in_relro(va);
    if (g_cow && relro && !g_cowed.count(va & ~0xfffull)) e->perms = UC_PROT_READ | UC_PROT_EXEC;
    else if (g_noexec && relro) e->perms = UC_PROT_READ | UC_PROT_WRITE;   // RW, NO EXEC
    else e->perms = UC_PROT_ALL;
    return true;
}
static bool g_trace = false;
static uint32_t sys_cb(uc_engine* uc, int, const void* cp_reg, void*) {
    auto* cp = static_cast<const uc_arm64_cp_reg*>(cp_reg);
    if (cp && cp->crn == 8) {
        uint64_t fa = 0; uc_reg_read(uc, UC_ARM64_REG_FAR_EL1, &fa);
        uint64_t elr = 0; uc_reg_read(uc, UC_ARM64_REG_ELR_EL1, &elr);
        if (g_trace && fa >= BASE + 0xd6000 && fa < BASE + 0xd7000) {
            uint64_t es[24] = {0}; uc_arm64_exec_state(uc, es);
            std::printf("  === COW_FAULT far=%llx elr=+%llx r3=%08x IT=%02x cpsr=%08x ===\n",
                        (unsigned long long)fa, (unsigned long long)(elr - FUNC_VA),
                        (unsigned)es[5+3], (unsigned)es[21], (unsigned)es[22]);
        }
        g_cowed.insert(fa & ~0xfffull); uc_ctl_flush_tlb(uc);
    }
    return 0;
}
static const uint8_t kEret[] = { 0xe0,0x03,0x9f,0xd6 };
static const uint8_t kVec[]  = { 0x1f,0x87,0x08,0xd5, 0xe0,0x03,0x9f,0xd6 };
static const uint8_t kSpin[] = { 0xfe,0xe7 };           // thumb: b .

static int run(const std::vector<uint8_t>& lk, bool cow) {
    g_cow = cow; g_cowed.clear();
    uc_engine* uc=nullptr;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN, &uc)) { std::printf("uc_open fail\n"); return 2; }
    uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
    uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL);  uc_mem_write(uc, 0x1000, kEret, sizeof kEret);
    uc_mem_write(uc, 0x1500, kSpin, sizeof kSpin);                       // return trampoline
    uc_mem_map(uc, 0x6000, 0x2000, UC_PROT_ALL);                        // stack
    uc_mem_map(uc, 0x8000, 0x1000, UC_PROT_ALL);  uc_mem_write(uc, 0x8600, kVec, sizeof kVec);
    uc_mem_map(uc, SOINFO, 0x1000, UC_PROT_ALL);
    uint32_t vb=(uint32_t)BASE, ptr=(uint32_t)RELR_VA, cnt=RELR_SZ/4;
    uc_mem_write(uc, SOINFO+0x11c, &vb, 4);
    uc_mem_write(uc, SOINFO+0x1b8, &ptr, 4);
    uc_mem_write(uc, SOINFO+0x1bc, &cnt, 4);

    // real .relr.dyn (first RO LOAD segment) + real function code page
    uc_mem_map(uc, BASE, 0x1000, UC_PROT_ALL);
    uc_mem_write(uc, RELR_VA, lk.data()+0x710, RELR_SZ);
    uc_mem_map(uc, FUNC_VA & ~0xfffull, 0x2000, UC_PROT_ALL);
    uc_mem_write(uc, FUNC_VA, lk.data()+FUNC_FOFF, FUNC_SZ);
    // real GNU_RELRO target region
    uint64_t rlo = RELRO_VA & ~0xfffull, rhi = (RELRO_VA + RELRO_MEMSZ + 0xfff) & ~0xfffull;
    uc_mem_map(uc, rlo, rhi - rlo, UC_PROT_ALL);
    std::vector<uint8_t> relro(RELRO_MEMSZ, 0);
    std::memcpy(relro.data(), lk.data()+0xcf950, RELRO_FILESZ);
    uc_mem_write(uc, RELRO_VA, relro.data(), RELRO_MEMSZ);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_TLB_FILL, (void*)tlb_cb, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INSN, (void*)sys_cb, nullptr, 1, 0, UC_ARM64_INS_SYS);
    g_trace = std::getenv("HOLLYWOOD_TRACE_STORE") != nullptr;
    if (g_trace) {
        uc_hook ch;
        uc_hook_add(uc, &ch, UC_HOOK_CODE, (void*)code_cb, nullptr, LOOP_LO, LOOP_HI);
    }
    if (std::getenv("HOLLYWOOD_SINGLESTEP")) {   // force single-instruction TBs globally
        uc_hook sh;
        uc_hook_add(uc, &sh, UC_HOOK_CODE, (void*)nop_cb, nullptr, 1, 0);
    }

    uint64_t pstate=0x3c5, spsr=0x30, elr=FUNC_VA, vbar=0x8000;
    uc_reg_write(uc, UC_ARM64_REG_PSTATE,&pstate); uc_reg_write(uc, UC_ARM64_REG_SPSR_EL1,&spsr);
    uc_reg_write(uc, UC_ARM64_REG_ELR_EL1,&elr);   uc_reg_write(uc, UC_ARM64_REG_VBAR_EL1,&vbar);
    uint64_t x0=SOINFO, sp=0x7f00, lr=0x1501;                           // r0=this, sp, lr=thumb trampoline
    uc_reg_write(uc, UC_ARM64_REG_X0,&x0);
    uc_reg_write(uc, UC_ARM64_REG_SP,&sp);                              // AArch32 SP aliases X-reg banks; also set below
    uc_reg_write(uc, UC_ARM64_REG_X13,&sp);
    uc_reg_write(uc, UC_ARM64_REG_X14,&lr);

    uc_err e = uc_emu_start(uc, 0x1000, 0, 0, 5000000);
    uint32_t t9=0,t10=0,t11=0; uint64_t pc=0;
    uc_mem_read(uc, TABLE9,&t9,4); uc_mem_read(uc, TABLE10,&t10,4); uc_mem_read(uc, TABLE11,&t11,4);
    uc_reg_read(uc, UC_ARM64_REG_PC,&pc);
    std::printf("[%s] emu=%-24s pc=%llx\n", cow?"cow ":"plain", e?uc_strerror(e):"ok", (unsigned long long)pc);
    bool bad = (t10==0x881ed || t10==0x881ec);
    std::printf("       table[9]=%08x  table[10]=%08x  table[11]=%08x  -> table[10] %s\n",
                t9,t10,t11, bad?"**UNRELOCATED — REPRODUCED**":(t10>0xf7000000u?"relocated OK":"?"));
    uc_close(uc);
    return bad?1:0;
}
int main() {
    FILE* fp=fopen("C:\\Users\\drych\\AppData\\Local\\Temp\\claude_p9\\rt32\\linker","rb");
    if(!fp){ std::printf("no linker\n"); return 2; }
    fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
    std::vector<uint8_t> lk(n); fread(lk.data(),1,n,fp); fclose(fp);
    g_noexec = std::getenv("HOLLYWOOD_NOEXEC") != nullptr;
    std::printf("REAL relocate_relr @%llx  target table[10]=%llx  (172 relr entries) noexec=%d\n",
                (unsigned long long)FUNC_VA,(unsigned long long)TABLE10, (int)g_noexec);
    int a=run(lk,false), b=run(lk,true);
    std::printf("\nRESULT: plain=%s  cow=%s\n", a?"REPRODUCED":"ok", b?"REPRODUCED":"ok");
    return (a||b)?1:0;
}
