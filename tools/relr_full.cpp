// Full-context AArch32 RELR reproducer (Phase 9).
// Loads the REAL linker's .relr.dyn (172 entries / 3564 relocations) and the REAL
// GNU_RELRO segment content at the REAL VAs (load bias 0xf7982000), then runs a
// *correct* bionic RELR loop in AArch32-EL0 over the whole set. Configs:
//   plain : GNU_RELRO pages RW throughout.
//   cow   : GNU_RELRO pages RO until first write (guest write-prot fault -> COW ->
//           RW), matching the real relocation sequence (page 0xd6000 first written
//           at seq#0, long before table[10] at seq#35).
// Verdict question: does the FULL real context skip table[10] (0xf7a5808c) with a
// correct loop? If yes -> a context/COW/TLB backend bug. If no -> the real linker
// CODE is implicated, not the loop.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>
#include <string>

static const uint64_t BASE = 0xf7982000ull;
static const uint64_t RELR_VA = BASE + 0x710;        // .relr.dyn
static const uint32_t RELR_SZ = 0x2b0;
static const uint64_t RELRO_VA = BASE + 0xd1950;     // GNU_RELRO start
static const uint32_t RELRO_FILESZ = 0x4cb4, RELRO_MEMSZ = 0x56b0;
static const uint64_t TABLE10 = BASE + 0xd608c;

static bool g_cow = false;
static std::set<uint64_t> g_cowed;                   // pages made writable

static bool in_relro(uint64_t va){ return va >= (RELRO_VA & ~0xfffull) && va < ((RELRO_VA + RELRO_MEMSZ + 0xfff) & ~0xfffull); }

static bool tlb_cb(uc_engine*, uint64_t va, int /*type*/, void* result, void*) {
    struct Ent { uint64_t paddr; uint32_t perms; }* e = (Ent*)result;
    e->paddr = va;                                   // identity
    if (g_cow && in_relro(va) && !g_cowed.count(va & ~0xfffull)) e->perms = UC_PROT_READ | UC_PROT_EXEC;
    else e->perms = UC_PROT_ALL;
    return true;
}
static uint32_t sys_cb(uc_engine* uc, int, const void* cp_reg, void*) {
    auto* cp = static_cast<const uc_arm64_cp_reg*>(cp_reg);
    if (cp && cp->crn == 8) {                         // TLBI -> mark faulting page COW'd + flush
        uint64_t faddr = 0; uc_reg_read(uc, UC_ARM64_REG_FAR_EL1, &faddr);
        g_cowed.insert(faddr & ~0xfffull);
        uc_ctl_flush_tlb(uc);
    }
    return 0;
}

// AArch64 eret stub @0x1000.
static const uint8_t kEret[] = { 0xe0,0x03,0x9f,0xd6 };
// VBAR+0x600 handler: tlbi vmalle1 ; eret
static const uint8_t kVec[]  = { 0x1f,0x87,0x08,0xd5, 0xe0,0x03,0x9f,0xd6 };
// Full bionic RELR loop @0x2000 (Thumb). r0=relr r1=end r2=load_bias r3=where.
static const uint8_t kLoop[] = {
 0x88,0x42, 0x19,0xd2, 0x04,0x68, 0x04,0x30, 0x14,0xf0,0x01,0x0f, 0x05,0xd1,
 0x13,0x19, 0x1e,0x68, 0x16,0x44, 0x1e,0x60, 0x04,0x33, 0xf2,0xe7,
 0x00,0x25, 0x64,0x08, 0x08,0xd0, 0xe6,0x07, 0x04,0xd5,
 0x53,0xf8,0x25,0x60, 0x16,0x44, 0x43,0xf8,0x25,0x60, 0x01,0x35, 0xf4,0xe7,
 0x03,0xf1,0x7c,0x03, 0xe3,0xe7, 0xfe,0xe7,
};

static int run(const std::vector<uint8_t>& lk, bool cow) {
    g_cow = cow; g_cowed.clear();
    uc_engine* uc=nullptr;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN, &uc)) { std::printf("uc_open fail\n"); return 2; }
    uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
    uc_mem_map(uc, 0x1000, 0x4000, UC_PROT_ALL);
    uc_mem_map(uc, 0x1000 & ~0xfffull, 0, UC_PROT_ALL);   // no-op guard
    uc_mem_write(uc, 0x1000, kEret, sizeof kEret);
    uc_mem_write(uc, 0x2000, kLoop, sizeof kLoop);
    uc_mem_map(uc, 0x8000, 0x1000, UC_PROT_ALL);
    uc_mem_write(uc, 0x8600, kVec, sizeof kVec);          // VBAR=0x8000 -> +0x600

    // Map + load .relr.dyn page(s).
    uc_mem_map(uc, BASE & ~0xfffull, 0x2000, UC_PROT_ALL);
    uc_mem_write(uc, RELR_VA, lk.data() + 0x710, RELR_SZ);
    // Map + load GNU_RELRO.
    uint64_t rlo = RELRO_VA & ~0xfffull, rhi = (RELRO_VA + RELRO_MEMSZ + 0xfff) & ~0xfffull;
    uc_mem_map(uc, rlo, rhi - rlo, UC_PROT_ALL);
    std::vector<uint8_t> relro(RELRO_MEMSZ, 0);
    std::memcpy(relro.data(), lk.data() + 0xcf950, RELRO_FILESZ);
    uc_mem_write(uc, RELRO_VA, relro.data(), RELRO_MEMSZ);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_TLB_FILL, (void*)tlb_cb, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INSN, (void*)sys_cb, nullptr, 1, 0, UC_ARM64_INS_SYS);

    uint64_t pstate=0x3c5, spsr=0x30, elr=0x2000, vbar=0x8000;
    uc_reg_write(uc, UC_ARM64_REG_PSTATE,&pstate); uc_reg_write(uc, UC_ARM64_REG_SPSR_EL1,&spsr);
    uc_reg_write(uc, UC_ARM64_REG_ELR_EL1,&elr); uc_reg_write(uc, UC_ARM64_REG_VBAR_EL1,&vbar);
    uint64_t x0=RELR_VA, x1=RELR_VA+RELR_SZ, x2=BASE, x3=0;
    uc_reg_write(uc, UC_ARM64_REG_X0,&x0); uc_reg_write(uc, UC_ARM64_REG_X1,&x1);
    uc_reg_write(uc, UC_ARM64_REG_X2,&x2); uc_reg_write(uc, UC_ARM64_REG_X3,&x3);

    uc_err e = uc_emu_start(uc, 0x1000, 0, 0, 20000000);
    // read table[8..12] + spot-check total correctness
    uint32_t t[5]={}; uc_mem_read(uc, BASE+0xd6084, t, 20);
    std::printf("[%s] emu=%s  table[8..12] = %08x %08x [%08x] %08x %08x\n",
                cow?"cow ":"plain", e?uc_strerror(e):"ok", t[0],t[1],t[2],t[3],t[4]);
    uint32_t t10=0; uc_mem_read(uc, TABLE10, &t10, 4);
    bool bad = (t10 == 0x881ed || t10 == 0x881ec);
    std::printf("        table[10] @%llx = 0x%08x  %s\n", (unsigned long long)TABLE10, t10,
                bad ? "**UNRELOCATED (REPRODUCED)**" : (t10 > 0xf7000000u ? "relocated OK" : "??"));
    // count how many GNU_RELRO relative-reloc targets are still unrelocated (< base) as a sanity metric
    uc_close(uc);
    return bad ? 1 : 0;
}

int main() {
    FILE* fp = fopen("C:\\Users\\drych\\AppData\\Local\\Temp\\claude_p9\\rt32\\linker","rb");
    if (!fp) { std::printf("no linker file\n"); return 2; }
    fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
    std::vector<uint8_t> lk(n); fread(lk.data(),1,n,fp); fclose(fp);
    std::printf("linker %ld bytes; base=%llx table[10]=%llx\n", n, (unsigned long long)BASE, (unsigned long long)TABLE10);
    int a = run(lk, false);
    int b = run(lk, true);
    std::printf("\nRESULT: plain=%s  cow=%s\n", a?"REPRODUCED":"ok", b?"REPRODUCED":"ok");
    return (a||b)?1:0;
}
