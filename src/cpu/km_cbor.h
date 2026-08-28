// Minimal CBOR (RFC 8949) reader/writer for the QSEE keymaster TA wire protocol.
// The QTI keymaster@4.1 TA speaks CBOR: each command payload is a CBOR structure,
// keymaster tags are CBOR unsigned-int map keys, values are CBOR uints or byte
// strings. This codec supports only the major types that protocol uses:
//   0 unsigned int, 1 negative int, 2 byte string, 4 array, 5 map, 7 simple(bool).
// It is deliberately small + allocation-light; not a general CBOR library.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace km {

// ---- Writer -------------------------------------------------------------
struct CborWriter {
    std::vector<uint8_t> buf;

    void raw(uint8_t b) { buf.push_back(b); }
    // Emit a major-type header (major in [0,7]) with the given argument value,
    // choosing the shortest of the immediate / 1 / 2 / 4 / 8-byte forms.
    void head(uint8_t major, uint64_t v) {
        uint8_t m = (uint8_t)(major << 5);
        if (v < 24) { raw(m | (uint8_t)v); }
        else if (v <= 0xff) { raw(m | 24); raw((uint8_t)v); }
        else if (v <= 0xffff) { raw(m | 25); raw((uint8_t)(v >> 8)); raw((uint8_t)v); }
        else if (v <= 0xffffffffull) { raw(m | 26); for (int i = 3; i >= 0; --i) raw((uint8_t)(v >> (8 * i))); }
        else { raw(m | 27); for (int i = 7; i >= 0; --i) raw((uint8_t)(v >> (8 * i))); }
    }
    void u(uint64_t v) { head(0, v); }                       // unsigned int
    void nint(int64_t v) { head(1, (uint64_t)(-1 - v)); }    // negative int
    void boolean(bool b) { raw(b ? 0xf5 : 0xf4); }           // simple true/false
    void null_() { raw(0xf6); }
    void array(size_t n) { head(4, (uint64_t)n); }
    void map(size_t n) { head(5, (uint64_t)n); }
    void bytes(const uint8_t* p, size_t n) {
        head(2, (uint64_t)n);
        buf.insert(buf.end(), p, p + n);
    }
    void bytes(const std::vector<uint8_t>& v) { bytes(v.data(), v.size()); }
};

// ---- Reader -------------------------------------------------------------
// A cursor over a CBOR buffer. Each read* returns false on malformed / short
// input so callers can fail cleanly instead of reading past the end.
struct CborReader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;

    CborReader() = default;
    CborReader(const uint8_t* d, size_t n) : p(d), end(d + n) {}

    bool eof() const { return p >= end; }

    // Read one header: major type + argument value + (for bytes/text/array/map)
    // its length/count. Advances past the header bytes only.
    bool head(uint8_t& major, uint64_t& val) {
        if (p >= end) return false;
        uint8_t ib = *p++;
        major = ib >> 5;
        uint8_t ai = ib & 0x1f;
        if (ai < 24) { val = ai; return true; }
        int n;
        if (ai == 24) n = 1; else if (ai == 25) n = 2;
        else if (ai == 26) n = 4; else if (ai == 27) n = 8;
        else return false;                                   // indefinite/reserved unsupported
        if (end - p < n) return false;
        val = 0;
        for (int i = 0; i < n; ++i) val = (val << 8) | *p++;
        return true;
    }

    // Read an unsigned int item.
    bool u(uint64_t& v) { uint8_t m; return head(m, v) && m == 0; }
    // Read a signed int item (major 0 or 1).
    bool i(int64_t& v) {
        uint8_t m; uint64_t a; if (!head(m, a)) return false;
        if (m == 0) { v = (int64_t)a; return true; }
        if (m == 1) { v = -1 - (int64_t)a; return true; }
        return false;
    }
    // Read a byte string; sets ptr+len into the underlying buffer (no copy).
    bool bytes(const uint8_t*& out, size_t& len) {
        uint8_t m; uint64_t a; if (!head(m, a) || m != 2) return false;
        if ((uint64_t)(end - p) < a) return false;
        out = p; len = (size_t)a; p += a; return true;
    }
    bool bytes(std::vector<uint8_t>& out) {
        const uint8_t* o; size_t l; if (!bytes(o, l)) return false;
        out.assign(o, o + l); return true;
    }
    // Read an array header; returns the element count.
    bool array(uint64_t& n) { uint8_t m; return head(m, n) && m == 4; }
    // Read a map header; returns the pair count.
    bool map(uint64_t& n) { uint8_t m; return head(m, n) && m == 5; }

    // Skip one complete item (recursing into arrays/maps). Used to ignore
    // fields we don't consume without losing sync.
    bool skip() {
        uint8_t m; uint64_t a; if (!head(m, a)) return false;
        switch (m) {
        case 0: case 1: return true;                         // int
        case 2: case 3:                                      // bytes / text
            if ((uint64_t)(end - p) < a) return false; p += a; return true;
        case 4:                                              // array
            for (uint64_t k = 0; k < a; ++k) if (!skip()) return false; return true;
        case 5:                                              // map
            for (uint64_t k = 0; k < a; ++k) { if (!skip() || !skip()) return false; } return true;
        case 7: return true;                                 // simple/bool/null
        default: return false;
        }
    }
};

} // namespace km
