// AArch32 RELR reproducer (Phase 9) -- FAITHFUL to hollywood_emu's backend:
// UC_TLB_VIRTUAL + a TLB-fill hook that returns page perms + guest-delivered
// permission faults (write-prot) + a SYS hook that flushes the vTLB on TLBI.
// A COW page (RO on first write-fill, RW after) models .data.rel.ro; the guest
// abort handler does `tlbi; eret` to retry. The AArch32 RELR bitmap loop
// (entry=0x0f) relocates 3 consecutive words; we check whether the store whose
// page COW-faults loses its write.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>

static const uint8_t kEretStub[] = { 0xe0,0x03,0x9f,0xd6 };            // 0x1000: eret
static const uint8_t kThumb[] = {                                      // 0x2000: RELR loop
    0x00,0x23, 0x40,0x08, 0x08,0xd0, 0xc4,0x07, 0x04,0xd5,
    0x51,0xf8,0x23,0x50, 0x15,0x44, 0x41,0xf8,0x23,0x50,
    0x01,0x33, 0xf4,0xe7, 0xfe,0xe7,
};
// EL1 sync-from-lower-AArch32 vector (VBAR+0x600): tlbi vmalle1 ; eret
static const uint8_t kVec[] = { 0x1f,0x87,0x08,0xd5, 0xe0,0x03,0x9f,0xd6 };

static const uint64_t DATA = 0x6000;
static bool g_cow_done = false;   // data page RO until the first fault's TLBI, then RW

// Match hollywood_emu's tlb_cb exactly: ignores access type, returns PTE perms.
static bool tlb_cb(uc_engine*, uint64_t va, int /*type*/, void* result, void*) {
    struct Ent { uint64_t paddr; uint32_t perms; }* e = (Ent*)result;
    e->paddr = va;                                   // identity
    if ((va & ~0xfffULL) == DATA && !g_cow_done) e->perms = UC_PROT_READ | UC_PROT_EXEC; // RO
    else e->perms = UC_PROT_ALL;
    return true;
}
// Match hollywood_emu's sys_cb: flush vTLB on TLBI (CRn==8). Also model the COW
// completing (guest kernel would have made the page writable) at that point.
static uint32_t sys_cb(uc_engine* uc, int /*reg*/, const void* cp_reg, void*) {
    auto* cp = static_cast<const uc_arm64_cp_reg*>(cp_reg);
    if (cp && cp->crn == 8) { g_cow_done = true; uc_ctl_flush_tlb(uc); }
    return 0;
}

int main() {
    uc_engine* uc=nullptr;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN, &uc)) return 2;
    if (uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL)) std::printf("WARN: tlb_mode failed\n");
    const uint64_t CODE=0x1000, ATH=0x2000, VBAR=0x4000, BIAS=0x10000;
    uc_mem_map(uc, CODE, 0x4000, UC_PROT_ALL);
    uc_mem_map(uc, VBAR, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, DATA, 0x1000, UC_PROT_ALL);
    uc_mem_write(uc, CODE, kEretStub, sizeof kEretStub);
    uc_mem_write(uc, ATH,  kThumb, sizeof kThumb);
    uc_mem_write(uc, VBAR+0x600, kVec, sizeof kVec);
    uint32_t t[3]={0x100,0x200,0x300}; uc_mem_write(uc, DATA, t, 12);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_TLB_FILL, (void*)tlb_cb, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INSN, (void*)sys_cb, nullptr, 1, 0, UC_ARM64_INS_SYS);

    uint64_t pstate=0x3c5, spsr=0x30, elr=ATH, vbar=VBAR;
    uc_reg_write(uc, UC_ARM64_REG_PSTATE,&pstate); uc_reg_write(uc, UC_ARM64_REG_SPSR_EL1,&spsr);
    uc_reg_write(uc, UC_ARM64_REG_ELR_EL1,&elr);   uc_reg_write(uc, UC_ARM64_REG_VBAR_EL1,&vbar);
    uint64_t x0=0xf,x1=DATA,x2=BIAS;
    uc_reg_write(uc, UC_ARM64_REG_X0,&x0); uc_reg_write(uc, UC_ARM64_REG_X1,&x1); uc_reg_write(uc, UC_ARM64_REG_X2,&x2);

    uc_err e = uc_emu_start(uc, CODE, 0, 0, 800);
    if (e) std::printf("emu: %s\n", uc_strerror(e));
    uint32_t o[3]={}; uc_mem_read(uc, DATA, o, 12);
    const uint32_t exp[3]={0x100+BIAS,0x200+BIAS,0x300+BIAS};
    int bad=0; for(int i=0;i<3;i++){ bool ok=o[i]==exp[i]; if(!ok)bad++;
        std::printf("  where[%d]=0x%08x exp 0x%08x %s\n",i,o[i],exp[i],ok?"OK":"**LOST**"); }
    std::printf("cow_done=%d ; %s\n", g_cow_done, bad?"REPRODUCED (COW store lost)":"ok (all applied)");
    uc_close(uc);
    return bad?1:0;
}
