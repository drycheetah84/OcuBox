// Standalone kallsyms extractor for the raw arm64 kernel Image inside an
// Android boot image (v2). Emits "addr type name" lines sorted by address.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

static std::vector<uint8_t> readfile(const char* p) {
    FILE* f = fopen(p, "rb"); if (!f) { perror("open"); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(n); fread(b.data(), 1, n, f); fclose(f); return b;
}

int main(int argc, char** argv) {
    const char* img = argc > 1 ? argv[1] : "firmware/boot.img";
    const char* outp = argc > 2 ? argv[2] : "build/ksyms.txt";
    auto all = readfile(img);
    // Android boot img v2: page_size @36, kernel_size @8, kernel @ page_size.
    uint32_t ks = *(uint32_t*)&all[8];
    uint32_t ps = *(uint32_t*)&all[36];
    const uint8_t* k = all.data() + ps;
    size_t klen = ks;
    printf("kernel size=%u page=%u\n", ks, ps);

    auto U16 = [&](size_t o){ return (uint16_t)(k[o] | (k[o+1]<<8)); };
    auto U64 = [&](size_t o){ uint64_t v=0; memcpy(&v,k+o,8); return v; };

    // Enumerate ALL plausible token_index candidates, then fully validate each by
    // locating token_table + num_syms; accept the first that decodes completely.
    std::vector<size_t> cands;
    for (size_t p = 0; p + 512 < klen; p += 2) {
        if (k[p] != 0 || k[p+1] != 0) continue;
        bool ok = true; uint16_t prev = 0; bool alldelta1 = true;
        for (int m = 1; m < 256; m++) {
            uint16_t v = U16(p + m*2); int d = (int)v - (int)prev;
            if (d < 1 || d > 40) { ok = false; break; } if (d != 1) alldelta1 = false; prev = v;
        }
        if (!ok || alldelta1) continue;
        uint16_t last = U16(p + 255*2);
        if (last < 300 || last > 1600) continue;   // real token table spans ~600-1000 bytes
        cands.push_back(p);
    }
    printf("token_index candidates: %zu\n", cands.size());

    uint64_t N = 0; size_t numoff = 0, namesStart = 0, tt = 0, ti = 0, offsetsStart_global = 0;
    std::vector<std::string> names; std::string tok[256];
    for (size_t cti : cands) {
        uint16_t idx255 = U16(cti + 255*2);
        // token_table precedes token_index. Locate its start by the null-separator
        // invariant: for every token m>=1, the byte at start+idx[m]-1 is a '\0'
        // (terminator of token m-1), and start-1 is also '\0'.
        size_t ctt = 0;
        for (size_t S = (cti > idx255 + 300 ? cti - idx255 - 300 : 0); S + idx255 < cti; S++) {
            if (S == 0 || k[S-1] != 0) continue;
            bool ok = true;
            for (int m = 1; m < 256 && ok; m++) if (k[S + U16(cti + m*2) - 1] != 0) ok = false;
            // and the char at S..(first token) is non-null (token0 present or empty allowed)
            if (ok) { ctt = S; break; }
        }
        if (!ctt) continue;
        std::string ctok[256];
        for (int m = 0; m < 256; m++) {
            size_t off = ctt + U16(cti + m*2); size_t e = off; while (k[e]) e++;
            ctok[m].assign((const char*)k + off, e - off);
        }
        // markers[] (8-byte cumulative name offsets) sit right before token_table:
        // a long monotonic run starting at value 0 and reaching a large total.
        // Find the run start M closest to ctt with length>=100 and end>100000.
        size_t M = 0;
        for (size_t o = (ctt > 0x40000 ? ctt - 0x40000 : 0); o + 8 < ctt; o += 8) {
            if (U64(o) != 0) continue;
            size_t q = o; uint64_t pv = 0, len = 0;
            while (q + 8 <= ctt) { uint64_t v = U64(q); if (v < pv) break; pv = v; q += 8; len++; }
            if (len >= 100 && pv > 100000) M = o;   // keep the last (closest to ctt)
        }
        if (!M) continue;
        namesStart = 0; N = 0;
        // kallsyms_offsets: the longest run of non-decreasing s32 in the region before
        // token_table. The run's tail includes relative_base's low word; the run breaks
        // at relative_base's (negative) high word. Layout: offsets, relative_base(8),
        // num_syms(8), names.
        auto S32 = [&](size_t o){ return *(int32_t*)&k[o]; };
        size_t bestEnd = 0, bestLen = 0;
        { size_t lo = (M > 0x800000 ? M - 0x800000 : 0);
          for (size_t o = lo; o + 4 < M; ) {
            int32_t v0 = S32(o);
            if (v0 >= 0 && v0 < 0x1000) {
                size_t q = o; int32_t pv = -1; size_t len = 0;
                while (q + 4 < M) { int32_t v = S32(q); if (v < pv) break; pv = v; q += 4; len++; }
                if (len > bestLen) { bestLen = len; bestEnd = q; }
                o = q + 4;
            } else o += 4;
          }
        }
        if (bestLen < 20000) continue;
        // Globals are 256-byte aligned; names follows offsets + relative_base + num_syms
        // padding. Find names_start = first aligned pos decoding to valid symbols.
        auto valid_type = [](char c){ return strchr("tTdDbBrRwWaAvVnN", c) != nullptr; };
        size_t nstart = 0;
        for (size_t cand = (bestEnd + 7) & ~(size_t)7; cand + 64 < M; cand += 8) {
            size_t pos = cand; bool good = true;
            for (int s = 0; s < 6 && good; s++) {
                uint8_t len = k[pos++]; if (len < 2 || len > 64) { good = false; break; }
                std::string nm; for (int c = 0; c < len; c++) nm += ctok[k[pos + c]]; pos += len;
                if (!valid_type(nm[0])) good = false;
                for (char ch : nm) if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) good = false;
            }
            if (good) { nstart = cand; break; }
        }
        if (!nstart) continue;
        namesStart = nstart;
        // decode names until we reach markers_start M -> that count is N.
        { std::vector<std::string> tmp; size_t pos = namesStart;
          while (pos < M) { uint8_t len = k[pos++]; std::string nm;
              for (int c = 0; c < len; c++) nm += ctok[k[pos + c]]; pos += len; tmp.push_back(std::move(nm)); }
          N = tmp.size(); names = std::move(tmp);
          numoff = 0; tt = ctt; ti = cti; for (int m = 0; m < 256; m++) tok[m] = ctok[m];
          // offsets end at bestEnd (one past last), N entries -> offsets_start.
          offsetsStart_global = bestEnd - 4 * N;
          printf("[dbg] names_start=0x%zx N=%llu offsets_start=0x%zx off[0]=%d\n",
                 namesStart, (unsigned long long)N, offsetsStart_global, S32(offsetsStart_global));
        }
        if (N) break;
    }
    if (!N) { printf("num_syms not found\n"); return 1; }
    printf("token_index @ 0x%zx token_table @ 0x%zx\n", ti, tt);
    printf("num_syms N=%llu @ file 0x%zx\n", (unsigned long long)N, numoff);

    const uint64_t relbase = 0xffffff8008080000ull;   // _text (load PA 0x80080000)
    size_t offsetsStart = offsetsStart_global;
    printf("relative_base=0x%016llx  offsets @ file 0x%zx\n",
           (unsigned long long)relbase, offsetsStart);

    std::vector<std::pair<uint64_t, std::string>> syms; syms.reserve(N);
    for (uint64_t s = 0; s < N; s++) {
        int32_t off = *(int32_t*)&k[offsetsStart + s*4];
        uint64_t addr = relbase + (uint32_t)off;
        const std::string& nm = names[s]; if (nm.size() < 2) continue;
        char type = nm[0]; std::string name = nm.substr(1);
        char line[8]; snprintf(line, sizeof line, "%c", type);
        syms.push_back({addr, std::string(line) + " " + name});
    }
    std::sort(syms.begin(), syms.end(), [](auto&a, auto&b){ return a.first < b.first; });
    FILE* out = fopen(outp, "wb");
    for (auto& s : syms) fprintf(out, "%016llx %s\n", (unsigned long long)s.first, s.second.c_str());
    fclose(out);
    printf("wrote %s (%zu symbols)\n", outp, syms.size());
    return 0;
}
