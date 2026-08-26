// Standalone AArch64 ELF disassembler for /init forensics.
// Usage: disasm_elf <elf> <start_va_hex> <count>
#include <capstone/capstone.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

static uint64_t rd(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <elf> <va_hex> <count>\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(sz); fread(b.data(), 1, sz, f); fclose(f);

    uint64_t va = strtoull(argv[2], nullptr, 16);
    uint64_t count = strtoull(argv[3], nullptr, 10);

    // Map VA -> file offset via PT_LOAD headers.
    uint64_t phoff = rd(b.data() + 32);
    uint16_t phentsize = rd16(b.data() + 54), phnum = rd16(b.data() + 56);
    uint64_t foff = 0; bool mapped = false;
    for (int i = 0; i < phnum; ++i) {
        const uint8_t* p = b.data() + phoff + i * phentsize;
        if (rd32(p) != 1) continue; // PT_LOAD
        uint64_t o = rd(p + 8), vaddr = rd(p + 16), filesz = rd(p + 32);
        if (va >= vaddr && va < vaddr + filesz) { foff = o + (va - vaddr); mapped = true; break; }
    }
    if (!mapped) { fprintf(stderr, "VA %#llx not in any PT_LOAD\n", (unsigned long long)va); return 1; }

    csh cs;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &cs) != CS_ERR_OK) { fprintf(stderr, "cs_open failed\n"); return 1; }
    const uint8_t* code = b.data() + foff;
    size_t codesz = count * 4;
    cs_insn* insn;
    size_t n = cs_disasm(cs, code, codesz, va, count, &insn);
    for (size_t i = 0; i < n; ++i)
        printf("  %#010llx: %08x  %s %s\n", (unsigned long long)insn[i].address,
               rd32(code + i*4), insn[i].mnemonic, insn[i].op_str);
    cs_free(insn, n);
    cs_close(&cs);
    return 0;
}
