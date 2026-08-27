// Minimal isolation, memory-verified. The guest does: conditional store of
// 0xdeadbeef to page r2, then load it back and store that loaded value to a
// separate RW result page r4. We read r4 (= what the CPU's own load saw). If r4 !=
// 0xdeadbeef the store was not visible to the CPU. Matrix: {IT-guarded,
// branch-guarded} x {RW page (no fault), RO->COW page (write faults)}.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <set>

static const uint64_t PAGE=0x9000, RES=0xa000;
static bool g_cow=false; static long g_faults=0; static std::set<uint64_t> g_cowed;
static bool tlb_cb(uc_engine*, uint64_t va, int, void* r, void*){
    struct E{uint64_t p;uint32_t pr;}* e=(E*)r; e->p=va;
    e->pr = (g_cow && va==PAGE && !g_cowed.count(PAGE)) ? (UC_PROT_READ|UC_PROT_EXEC) : UC_PROT_ALL;
    return true;
}
static uint32_t sys_cb(uc_engine* uc,int,const void* c,void*){
    auto* cp=static_cast<const uc_arm64_cp_reg*>(c);
    if(cp&&cp->crn==8){ g_faults++; uint64_t fa=0; uc_reg_read(uc,UC_ARM64_REG_FAR_EL1,&fa); g_cowed.insert(fa&~0xfffull); uc_ctl_flush_tlb(uc);} return 0;
}
static const uint8_t kEret[]={0xe0,0x03,0x9f,0xd6}, kVec[]={0x1f,0x87,0x08,0xd5,0xe0,0x03,0x9f,0xd6};
// movs r0,#0 ; it eq ; streq r1,[r2] ; ldr r3,[r2] ; str r3,[r4] ; b .
static const uint8_t kIT[]={ 0x00,0x20, 0x08,0xbf, 0x11,0x60, 0x13,0x68, 0x23,0x60, 0xfe,0xe7 };
// movs r0,#0 ; bne +0 ; str r1,[r2] ; ldr r3,[r2] ; str r3,[r4] ; b .
static const uint8_t kBR[]={ 0x00,0x20, 0x00,0xd1, 0x11,0x60, 0x13,0x68, 0x23,0x60, 0xfe,0xe7 };

static void run(const char* tag, const uint8_t* code, int len, bool cow){
    g_cow=cow; g_faults=0; g_cowed.clear();
    uc_engine* uc=nullptr; uc_open(UC_ARCH_ARM64,UC_MODE_LITTLE_ENDIAN,&uc); uc_ctl_tlb_mode(uc,UC_TLB_VIRTUAL);
    uc_mem_map(uc,0x1000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x1000,kEret,4);
    uc_mem_map(uc,0x2000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x2000,code,len);
    uc_mem_map(uc,0x8000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x8600,kVec,8);
    uc_mem_map(uc,PAGE,0x1000,UC_PROT_ALL); uint32_t orig=0x11111111; uc_mem_write(uc,PAGE,&orig,4);
    uc_mem_map(uc,RES,0x1000,UC_PROT_ALL); uint32_t z=0; uc_mem_write(uc,RES,&z,4);
    uc_hook h; uc_hook_add(uc,&h,UC_HOOK_TLB_FILL,(void*)tlb_cb,nullptr,1,0);
    uc_hook_add(uc,&h,UC_HOOK_INSN,(void*)sys_cb,nullptr,1,0,UC_ARM64_INS_SYS);
    uint64_t pstate=0x3c5,spsr=0x30,elr=0x2000,vbar=0x8000;
    uc_reg_write(uc,UC_ARM64_REG_PSTATE,&pstate);uc_reg_write(uc,UC_ARM64_REG_SPSR_EL1,&spsr);uc_reg_write(uc,UC_ARM64_REG_ELR_EL1,&elr);uc_reg_write(uc,UC_ARM64_REG_VBAR_EL1,&vbar);
    uint64_t r1=0xdeadbeef,r2=PAGE,r4=RES; uc_reg_write(uc,UC_ARM64_REG_X1,&r1);uc_reg_write(uc,UC_ARM64_REG_X2,&r2);uc_reg_write(uc,UC_ARM64_REG_X4,&r4);
    uc_err e=uc_emu_start(uc,0x1000,0,0,300);
    uint32_t res=0,phys=0; uc_mem_read(uc,RES,&res,4); uc_mem_read(uc,PAGE,&phys,4);
    std::printf("%-12s emu=%-16s faults=%-3ld  CPU-loaded=%08x  phys[page]=%08x  -> %s\n",
        tag, e?uc_strerror(e):"ok", g_faults, res, phys,
        (res==0xdeadbeef)?"OK":"** CPU LOAD SAW STALE (store lost to CPU) **");
    uc_close(uc);
}
int main(){
    run("IT  no-cow", kIT,sizeof kIT,false);
    run("BR  no-cow", kBR,sizeof kBR,false);
    run("IT  cow",    kIT,sizeof kIT,true);
    run("BR  cow",    kBR,sizeof kBR,true);
    return 0;
}
