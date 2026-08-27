// Phase 9F: full-image faithful RELR reproducer.
// Maps ALL PT_LOAD segments of the real 32-bit linker at the real load bias with
// per-segment permissions and independent per-page copy-on-write for writable
// segments (incl. the 4th LOAD, 0xd7620-0xe2a40, which relr_real never covered),
// runs the REAL soinfo::relocate_relr over the COMPLETE .relr.dyn, then verifies
// EVERY relr target:  expected = file_value + load_bias  vs  actual (relocated).
// Reports every mismatch with segment/page/original/expected/actual.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <set>
#include <vector>

static const uint64_t BASE = 0xf7982000ull;          // load bias
static const uint64_t FUNC_VA = BASE + 0x248d0;      // soinfo::relocate_relr
static const uint32_t FUNC_FOFF = 0x238d0, FUNC_SZ = 0x5a;
static const uint64_t SOINFO = 0x10000;
static const uint32_t RELR_FOFF = 0x710, RELR_SZ = 0x2b0;   // .relr.dyn (in seg1)

struct Seg { uint64_t va, off, filesz, memsz; uint32_t flags; };
static std::vector<Seg> g_segs;
static std::vector<uint8_t> g_lk;
static std::set<uint64_t> g_cowed;
static bool g_trace = false;

static const Seg* seg_of(uint64_t off) {   // off = linker-space offset (va)
    for (auto& s : g_segs) if (off >= s.va && off < s.va + s.memsz) return &s;
    return nullptr;
}
static bool page_is_rw_cow(uint64_t va) {   // va = guest VA (BASE+off)
    if (va < BASE) return false;            // low helper pages
    const Seg* s = seg_of(va - BASE);
    return s && (s->flags & 2u);            // PF_W
}
static bool in_image(uint64_t va) { return va >= BASE && seg_of(va - BASE) != nullptr; }

static bool tlb_cb(uc_engine*, uint64_t va, int, void* r, void*) {
    struct E { uint64_t p; uint32_t pr; }* e = (E*)r;
    e->p = va;                              // identity
    if (page_is_rw_cow(va)) {
        e->pr = g_cowed.count(va & ~0xfffull) ? (UC_PROT_READ|UC_PROT_WRITE|UC_PROT_EXEC)
                                              : (UC_PROT_READ|UC_PROT_EXEC);   // RO until COW
    } else if (in_image(va)) {
        e->pr = UC_PROT_READ | UC_PROT_EXEC;                                    // RO/RX segments
    } else {
        e->pr = UC_PROT_ALL;                                                    // stub/stack/soinfo/vbar
    }
    return true;
}
static long g_faults = 0;
static uint32_t sys_cb(uc_engine* uc, int, const void* c, void*) {
    auto* cp = static_cast<const uc_arm64_cp_reg*>(c);
    if (cp && cp->crn == 8) {
        uint64_t fa = 0; uc_reg_read(uc, UC_ARM64_REG_FAR_EL1, &fa);
        if (g_trace && fa >= BASE+0xd6000 && fa < BASE+0xd7000) {
            uint64_t es[24]={0}; uc_arm64_exec_state(uc, es);
            uint64_t spsr=0; uc_reg_read(uc, UC_ARM64_REG_SPSR_EL1, &spsr);
            // AArch32 ITSTATE packed in CPSR/SPSR: IT[7:2]=bits[15:10], IT[1:0]=bits[26:25]
            unsigned it = (unsigned)(((spsr>>8)&0xfc) | ((spsr>>25)&0x3));
            std::printf("  <FAULT far=%llx r3=%08x IT=%02x  SPSR=%08x SPSR.IT=%02x>\n",
                (unsigned long long)fa,(unsigned)es[5+3],(unsigned)es[21],(unsigned)spsr,it);
        }
        g_cowed.insert(fa & ~0xfffull); uc_ctl_flush_tlb(uc); g_faults++;
    }
    return 0;
}
static const uint8_t kEret[]={0xe0,0x03,0x9f,0xd6}, kVec[]={0x1f,0x87,0x08,0xd5,0xe0,0x03,0x9f,0xd6}, kSpin[]={0xfe,0xe7};

extern "C" void uc_arm64_exec_state(uc_engine*, uint64_t*);
static const uint64_t LOOP_LO = FUNC_VA + 0x38, STORE_PC = FUNC_VA + 0x44, LOOP_HI = FUNC_VA + 0x50;
static long g_seq = 0;
// trace the bitmap loop while r3 (target offset) is near page d6000 (d6000..d6010)
static void code_cb(uc_engine* uc, uint64_t pc, uint32_t, void*) {
    uint64_t es[24] = {0}; uc_arm64_exec_state(uc, es);
    uint64_t r2 = es[5+2], r3 = es[5+3], r4 = es[5+4], r5 = es[5+5];
    if (r3 < 0xd6000 || r3 > 0xd6014) return;
    const char* lbl = (pc==LOOP_LO)?"lsls":(pc==FUNC_VA+0x3a)?"IT":(pc==FUNC_VA+0x3c)?"ldr.bias":
        (pc==FUNC_VA+0x40)?"ldr r5":(pc==STORE_PC)?"STR":(pc==STORE_PC+2)?"adds r3":(pc==FUNC_VA+0x48)?"lsrs":"";
    std::printf("  [%03ld] pc=+%03x %-9s r2=%08x r3=%08x r5=%08x IT=%02x\n",
        g_seq++, (unsigned)(pc-FUNC_VA), lbl, (unsigned)r2,(unsigned)r3,(unsigned)r5,(unsigned)es[21]);
}
static uint32_t u32(uint32_t off){ uint32_t v; std::memcpy(&v, g_lk.data()+off, 4); return v; }

// original file value at linker-space va (0 in .bss, absent if outside any segment)
static bool orig_at(uint64_t va, uint32_t& out) {
    const Seg* s = seg_of(va);
    if (!s) return false;
    if (va < s->va + s->filesz) { std::memcpy(&out, g_lk.data() + s->off + (va - s->va), 4); return true; }
    out = 0; return true;   // .bss
}
static int seg_index(uint64_t va){ for (size_t i=0;i<g_segs.size();++i) if (va>=g_segs[i].va && va<g_segs[i].va+g_segs[i].memsz) return (int)i; return -1; }

int main() {
    FILE* fp=fopen("C:\\Users\\drych\\AppData\\Local\\Temp\\claude_p9\\rt32\\linker","rb");
    if(!fp){ std::printf("no linker\n"); return 2; }
    fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
    g_lk.resize(n); fread(g_lk.data(),1,n,fp); fclose(fp);

    // parse Elf32 PT_LOAD segments
    uint32_t e_phoff=u32(28); uint16_t e_phentsize, e_phnum;
    std::memcpy(&e_phentsize, g_lk.data()+42, 2); std::memcpy(&e_phnum, g_lk.data()+44, 2);
    for (int i=0;i<e_phnum;i++){ uint32_t o=e_phoff+i*e_phentsize; if (u32(o)==1) // PT_LOAD
        g_segs.push_back({u32(o+8), u32(o+4), u32(o+16), u32(o+20), u32(o+24)}); }
    std::printf("PT_LOAD segments (%zu):\n", g_segs.size());
    for (auto& s : g_segs) std::printf("  va=%llx-%llx off=%llx filesz=%llx memsz=%llx flags=%u%s\n",
        (unsigned long long)s.va,(unsigned long long)(s.va+s.memsz),(unsigned long long)s.off,
        (unsigned long long)s.filesz,(unsigned long long)s.memsz,s.flags,(s.flags&2)?" RW":"");

    uc_engine* uc=nullptr; uc_open(UC_ARCH_ARM64,UC_MODE_LITTLE_ENDIAN,&uc); uc_ctl_tlb_mode(uc,UC_TLB_VIRTUAL);
    uc_mem_map(uc,0x1000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x1000,kEret,4); uc_mem_write(uc,0x1500,kSpin,2);
    uc_mem_map(uc,0x6000,0x2000,UC_PROT_ALL); uc_mem_map(uc,0x8000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x8600,kVec,8);
    uc_mem_map(uc,SOINFO,0x1000,UC_PROT_ALL);
    uint32_t vb=(uint32_t)BASE, ptr=(uint32_t)(BASE+RELR_FOFF), cnt=RELR_SZ/4;
    uc_mem_write(uc,SOINFO+0x11c,&vb,4); uc_mem_write(uc,SOINFO+0x1b8,&ptr,4); uc_mem_write(uc,SOINFO+0x1bc,&cnt,4);

    // map the whole image span [BASE, BASE+end) and write each segment's file bytes
    uint64_t hi=0; for (auto& s:g_segs){ uint64_t e=(s.va+s.memsz+0xfff)&~0xfffull; if(e>hi)hi=e; }
    uc_mem_map(uc, BASE, hi, UC_PROT_ALL);
    for (auto& s : g_segs) uc_mem_write(uc, BASE+s.va, g_lk.data()+s.off, s.filesz);

    uc_hook h;
    uc_hook_add(uc,&h,UC_HOOK_TLB_FILL,(void*)tlb_cb,nullptr,1,0);
    uc_hook_add(uc,&h,UC_HOOK_INSN,(void*)sys_cb,nullptr,1,0,UC_ARM64_INS_SYS);
    g_trace = std::getenv("HOLLYWOOD_TRACE") != nullptr;
    if (g_trace) { uc_hook ch; uc_hook_add(uc,&ch,UC_HOOK_CODE,(void*)code_cb,nullptr,LOOP_LO,LOOP_HI); }

    uint64_t pstate=0x3c5,spsr=0x30,elr=FUNC_VA,vbar=0x8000;
    uc_reg_write(uc,UC_ARM64_REG_PSTATE,&pstate);uc_reg_write(uc,UC_ARM64_REG_SPSR_EL1,&spsr);
    uc_reg_write(uc,UC_ARM64_REG_ELR_EL1,&elr);uc_reg_write(uc,UC_ARM64_REG_VBAR_EL1,&vbar);
    uint64_t x0=SOINFO,sp=0x7f00,lr=0x1501; uc_reg_write(uc,UC_ARM64_REG_X0,&x0);
    uc_reg_write(uc,UC_ARM64_REG_X13,&sp);uc_reg_write(uc,UC_ARM64_REG_X14,&lr);

    uc_err e=uc_emu_start(uc,0x1000,0,0,50000000);
    uint64_t pc=0; uc_reg_read(uc,UC_ARM64_REG_PC,&pc);
    std::printf("\nrelocate_relr ran: emu=%s finalpc=%llx cow_faults=%ld\n", e?uc_strerror(e):"ok",(unsigned long long)pc,g_faults);

    // ---- verify EVERY relr target ----
    long total=0, bad=0; uint64_t where=0;
    std::set<uint64_t> written_pages;
    for (uint32_t p=RELR_FOFF; p<RELR_FOFF+RELR_SZ; p+=4) {
        uint32_t ent=u32(p);
        char kind = (ent&1)==0 ? 'A' : 'B';   // ADDR-handler vs BITMAP-handler store
        auto check=[&](uint64_t tva){
            total++;
            uint64_t pg=(BASE+tva)&~0xfffull;
            bool first = !written_pages.count(pg); written_pages.insert(pg);
            uint32_t orig=0; if(!orig_at(tva,orig)) { std::printf("  target %llx: OUTSIDE any segment\n",(unsigned long long)tva); bad++; return; }
            uint32_t expected=orig+(uint32_t)BASE, actual=0; uc_mem_read(uc, BASE+tva, &actual, 4);
            if (actual!=expected) {
                bad++;
                if (bad<=40) std::printf("  MISMATCH tva=%llx seg%d page=%llx handler=%c first-write-on-page=%s orig=%08x expected=%08x actual=%08x\n",
                    (unsigned long long)tva, seg_index(tva), (unsigned long long)pg, kind, first?"YES":"no", orig, expected, actual);
            }
        };
        if ((ent&1)==0) { where=ent; check(where); where+=4; }
        else { uint32_t bits=ent; long i=0; while ((bits>>=1)!=0){ if(bits&1) check(where+i*4); i++; } where+=31*4; }
    }
    std::printf("\nRELR VERIFY: %ld targets, %ld mismatches -> %s\n", total, bad, bad? "**RELOCATIONS SKIPPED**":"ALL CORRECT");
    uc_close(uc); return bad?1:0;
}
