// Instrumented cow-mode reproducer: traces every relocating store near
// table[9..11] (bitmap str @0x24914, ADDR str @0x248f2) and every write-prot
// fault (vector @0x8600, FAR). Reveals whether table[10]'s store executes, faults,
// retries, and whether the retried store lands.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

static const uint64_t BASE=0xf7982000ull, RELR_VA=BASE+0x710, RELRO_VA=BASE+0xd1950, FUNC_VA=BASE+0x248d0, SOINFO=0x10000;
static const uint32_t RELR_SZ=0x2b0, RELRO_FILESZ=0x4cb4, RELRO_MEMSZ=0x56b0, FUNC_FOFF=0x238d0, FUNC_SZ=0x5a;
static const uint64_t TABLE10=BASE+0xd608c;
static std::set<uint64_t> g_cowed;
static uint64_t g_pend=0;
static long g_faults=0, g_stores=0;

static bool in_relro(uint64_t va){ return va>=(RELRO_VA&~0xfffull) && va<((RELRO_VA+RELRO_MEMSZ+0xfff)&~0xfffull); }
static bool tlb_cb(uc_engine*, uint64_t va, int, void* r, void*){
    struct E{uint64_t p;uint32_t pr;}* e=(E*)r; e->p=va;
    e->pr = (in_relro(va)&&!g_cowed.count(va&~0xfffull)) ? (UC_PROT_READ|UC_PROT_EXEC) : UC_PROT_ALL;
    return true;
}
static uint32_t sys_cb(uc_engine* uc,int,const void* c,void*){
    auto* cp=static_cast<const uc_arm64_cp_reg*>(c);
    if(cp&&cp->crn==8){ uint64_t fa=0; uc_reg_read(uc,UC_ARM64_REG_FAR_EL1,&fa); g_cowed.insert(fa&~0xfffull); uc_ctl_flush_tlb(uc);} return 0;
}
static const uint64_t PG = BASE+0xd6000;   // table page
static void code_cb(uc_engine* uc, uint64_t pc, uint32_t, void*){
    if(g_pend){ uint32_t v=0; uc_mem_read(uc,g_pend,&v,4); std::printf("       -> after: [%llx]=%08x %s\n",(unsigned long long)g_pend,v, v>0xf7000000u?"(LANDED)":"(NOT landed)"); g_pend=0; }
    if(pc==BASE+0x24914){ // bitmap str r5,[r3,r4]
        uint64_t r3=0,r4=0,r5=0,nz=0; uc_reg_read(uc,UC_ARM64_REG_X3,&r3);uc_reg_read(uc,UC_ARM64_REG_X4,&r4);uc_reg_read(uc,UC_ARM64_REG_X5,&r5);uc_reg_read(uc,UC_ARM64_REG_NZCV,&nz);
        uint64_t t=(uint32_t)(r3+r4);
        if((t&~0xfffull)==PG){ g_stores++; std::printf("  [bmp-str] tgt=%llx r5=%08x N=%d\n",(unsigned long long)t,(uint32_t)r5,(int)((nz>>31)&1)); g_pend=t; }
    } else if(pc==BASE+0x248f2){ // ADDR str.w r3,[lr,r2]
        uint64_t lr=0,r2=0,r3=0; uc_reg_read(uc,UC_ARM64_REG_X14,&lr);uc_reg_read(uc,UC_ARM64_REG_X2,&r2);uc_reg_read(uc,UC_ARM64_REG_X3,&r3);
        uint64_t t=(uint32_t)(lr+r2);
        if((t&~0xfffull)==PG){ std::printf("  [adr-str] tgt=%llx r3=%08x\n",(unsigned long long)t,(uint32_t)r3); g_pend=t; }
    } else if(pc==0x8600){ uint64_t fa=0; uc_reg_read(uc,UC_ARM64_REG_FAR_EL1,&fa); g_faults++;
        std::printf("  <FAULT#%ld> far=%llx\n",g_faults,(unsigned long long)fa);
    }
}
static const uint8_t kEret[]={0xe0,0x03,0x9f,0xd6}, kVec[]={0x1f,0x87,0x08,0xd5,0xe0,0x03,0x9f,0xd6}, kSpin[]={0xfe,0xe7};

int main(){
    FILE* fp=fopen("C:\\Users\\drych\\AppData\\Local\\Temp\\claude_p9\\rt32\\linker","rb");
    if(!fp){std::printf("no linker\n");return 2;}
    fseek(fp,0,SEEK_END);long n=ftell(fp);fseek(fp,0,SEEK_SET);std::vector<uint8_t> lk(n);fread(lk.data(),1,n,fp);fclose(fp);
    uc_engine* uc=nullptr; uc_open(UC_ARCH_ARM64,UC_MODE_LITTLE_ENDIAN,&uc); uc_ctl_tlb_mode(uc,UC_TLB_VIRTUAL);
    uc_mem_map(uc,0x1000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x1000,kEret,4); uc_mem_write(uc,0x1500,kSpin,2);
    uc_mem_map(uc,0x6000,0x2000,UC_PROT_ALL); uc_mem_map(uc,0x8000,0x1000,UC_PROT_ALL); uc_mem_write(uc,0x8600,kVec,8);
    uc_mem_map(uc,SOINFO,0x1000,UC_PROT_ALL);
    uint32_t vb=(uint32_t)BASE,ptr=(uint32_t)RELR_VA,cnt=RELR_SZ/4;
    uc_mem_write(uc,SOINFO+0x11c,&vb,4); uc_mem_write(uc,SOINFO+0x1b8,&ptr,4); uc_mem_write(uc,SOINFO+0x1bc,&cnt,4);
    uc_mem_map(uc,BASE,0x1000,UC_PROT_ALL); uc_mem_write(uc,RELR_VA,lk.data()+0x710,RELR_SZ);
    uc_mem_map(uc,FUNC_VA&~0xfffull,0x2000,UC_PROT_ALL); uc_mem_write(uc,FUNC_VA,lk.data()+FUNC_FOFF,FUNC_SZ);
    uint64_t rlo=RELRO_VA&~0xfffull,rhi=(RELRO_VA+RELRO_MEMSZ+0xfff)&~0xfffull; uc_mem_map(uc,rlo,rhi-rlo,UC_PROT_ALL);
    std::vector<uint8_t> relro(RELRO_MEMSZ,0); std::memcpy(relro.data(),lk.data()+0xcf950,RELRO_FILESZ); uc_mem_write(uc,RELRO_VA,relro.data(),RELRO_MEMSZ);
    uc_hook h;
    uc_hook_add(uc,&h,UC_HOOK_TLB_FILL,(void*)tlb_cb,nullptr,1,0);
    uc_hook_add(uc,&h,UC_HOOK_INSN,(void*)sys_cb,nullptr,1,0,UC_ARM64_INS_SYS);
    uc_hook_add(uc,&h,UC_HOOK_CODE,(void*)code_cb,nullptr,1,0);
    uint64_t pstate=0x3c5,spsr=0x30,elr=FUNC_VA,vbar=0x8000; uc_reg_write(uc,UC_ARM64_REG_PSTATE,&pstate);uc_reg_write(uc,UC_ARM64_REG_SPSR_EL1,&spsr);uc_reg_write(uc,UC_ARM64_REG_ELR_EL1,&elr);uc_reg_write(uc,UC_ARM64_REG_VBAR_EL1,&vbar);
    uint64_t x0=SOINFO,sp=0x7f00,lr=0x1501; uc_reg_write(uc,UC_ARM64_REG_X0,&x0);uc_reg_write(uc,UC_ARM64_REG_X13,&sp);uc_reg_write(uc,UC_ARM64_REG_X14,&lr);
    uc_err e=uc_emu_start(uc,0x1000,0,0,5000000);
    uint32_t t9=0,t10=0,t11=0; uint64_t fpc=0; uc_mem_read(uc,BASE+0xd6088,&t9,4); uc_mem_read(uc,TABLE10,&t10,4); uc_mem_read(uc,BASE+0xd6090,&t11,4); uc_reg_read(uc,UC_ARM64_REG_PC,&fpc);
    std::printf("\nDONE emu=%s finalpc=%llx stores-on-page=%ld faults=%ld\n  table[9]=%08x table[10]=%08x table[11]=%08x -> table[10] %s\n",
        e?uc_strerror(e):"ok",(unsigned long long)fpc,g_stores,g_faults,t9,t10,t11,(t10==0x881ed)?"UNRELOCATED":"ok");
    uc_close(uc); return 0;
}
