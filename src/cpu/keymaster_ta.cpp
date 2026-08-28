#include "cpu/keymaster_ta.h"
#include "cpu/km_cbor.h"
#include "common/sha256.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Synthetic QSEE keymaster@4.1 TA. See keymaster_ta.h for the compatibility contract.
//
// WIRE PROTOCOL (RE'd from libqtikeymaster4.so KeymasterSerialize + KeymasterUtils::
// sendCmd; see tmp/km_wire_schema.md):
//   request  = <u32 LE cmd id = 0x2000|km_cmd> <CBOR body>
//   response = <int32 status @0> <u32 cbor_len @4> <CBOR body @8>   (status 0 = KM_OK)
// An AuthorizationSet is a CBOR map {22:N, tag:val...}: key 22 = param count, then N
// keymaster tags. A tag is the 32-bit tag as a CBOR int -- unsigned for type nibble 1-7,
// negative for 8/9/A (top bit set). Values: uint (int/enum/ulong/date), CBOR true (bool,
// presence=true), byte string (bytes/bignum). KeyCharacteristics = map{1:{hw},0:{sw}}
// (inner maps have NO 22 count). Key blob = CBOR array [0x4b4d4b44,u64,u64,bstr(chars),...].

namespace km {
namespace {

// ---- keymaster tag types + selected tag/enum ids ------------------------
constexpr uint32_t TYPE_MASK = 0xF0000000u;
constexpr uint32_t T_ENUM = 0x10000000u, T_ENUM_REP = 0x20000000u, T_UINT = 0x30000000u,
                   T_UINT_REP = 0x40000000u, T_ULONG = 0x50000000u, T_DATE = 0x60000000u,
                   T_BOOL = 0x70000000u, T_BIGNUM = 0x80000000u, T_BYTES = 0x90000000u,
                   T_ULONG_REP = 0xA0000000u;

constexpr uint32_t TAG_PURPOSE          = T_ENUM_REP | 1;    // 0x20000001
constexpr uint32_t TAG_ALGORITHM        = T_ENUM     | 2;    // 0x10000002
constexpr uint32_t TAG_KEY_SIZE         = T_UINT     | 3;    // 0x30000003
constexpr uint32_t TAG_BLOCK_MODE       = T_ENUM_REP | 4;    // 0x20000004
constexpr uint32_t TAG_DIGEST           = T_ENUM_REP | 5;    // 0x20000005
constexpr uint32_t TAG_PADDING          = T_ENUM_REP | 6;    // 0x20000006
constexpr uint32_t TAG_MIN_MAC_LENGTH   = T_UINT     | 8;    // 0x30000008
constexpr uint32_t TAG_MAC_LENGTH       = T_UINT     | 1003; // 0x300003eb
constexpr uint32_t TAG_NONCE            = T_BYTES    | 1001; // 0x900003e9
constexpr uint32_t TAG_ASSOCIATED_DATA  = T_BYTES    | 1000; // 0x900003e8
constexpr uint32_t TAG_APPLICATION_ID   = T_BYTES    | 601;  // 0x90000259
constexpr uint32_t TAG_APPLICATION_DATA = T_BYTES    | 700;  // 0x900002bc
constexpr uint32_t TAG_ROOT_OF_TRUST    = T_BYTES    | 704;  // 0x900002c0
constexpr uint32_t TAG_ORIGIN           = T_ENUM     | 702;  // 0x100002be
constexpr uint32_t TAG_OS_VERSION       = T_UINT     | 705;  // 0x300002c1
constexpr uint32_t TAG_OS_PATCHLEVEL    = T_UINT     | 706;  // 0x300002c2
constexpr uint32_t TAG_VENDOR_PATCHLEVEL= T_UINT     | 718;  // 0x300002ce
constexpr uint32_t TAG_CREATION_DATETIME= T_DATE     | 701;  // 0x600002bd

// KM_ALGORITHM
constexpr uint64_t ALG_RSA = 1, ALG_EC = 3, ALG_AES = 32, ALG_TRIPLE_DES = 33, ALG_HMAC = 128;
// KM_PURPOSE
constexpr uint64_t PURP_ENCRYPT = 0, PURP_DECRYPT = 1, PURP_SIGN = 2, PURP_VERIFY = 3,
                   PURP_WRAP = 5;
// KM_BLOCK_MODE
constexpr uint64_t BM_ECB = 1, BM_CBC = 2, BM_CTR = 3, BM_GCM = 32;
// KM ErrorCode (negative)
constexpr int32_t KM_OK = 0, KM_ERR_INVALID_OPERATION_HANDLE = -28,
                  KM_ERR_VERIFICATION_FAILED = -30, KM_ERR_UNIMPLEMENTED = -100,
                  KM_ERR_UNKNOWN = -1000, KM_ERR_INVALID_KEY_BLOB = -33;

inline bool is_bytes_type(uint32_t tag) {
    uint32_t t = tag & TYPE_MASK; return t == T_BIGNUM || t == T_BYTES;
}
inline bool is_bool_type(uint32_t tag) { return (tag & TYPE_MASK) == T_BOOL; }

// ---- parsed key parameter + authorization set ---------------------------
struct KeyParam {
    uint32_t tag = 0;
    uint64_t integer = 0;
    std::vector<uint8_t> blob;
};
using AuthSet = std::vector<KeyParam>;

const KeyParam* find(const AuthSet& s, uint32_t tag) {
    for (auto& p : s) if (p.tag == tag) return &p;
    return nullptr;
}

// Encode one keymaster tag:value pair into `w`.
void encode_param(CborWriter& w, const KeyParam& p) {
    if (p.tag & 0x80000000u) w.nint((int32_t)p.tag);   // BYTES/BIGNUM/ULONG_REP -> negative key
    else w.u(p.tag);
    if (is_bytes_type(p.tag)) w.bytes(p.blob);
    else if (is_bool_type(p.tag)) w.boolean(true);     // present BOOL == true
    else w.u(p.integer);
}

// Encode an AuthSet as the request/response params map {22:N, tag:val...}.
void encode_params(CborWriter& w, const AuthSet& s) {
    w.map(s.size() + 1);
    w.u(22); w.u(s.size());
    for (auto& p : s) encode_param(w, p);
}

// Encode an AuthSet as a bare characteristics sub-map {tag:val...} (no 22 count).
void encode_authset_plain(CborWriter& w, const AuthSet& s) {
    w.map(s.size());
    for (auto& p : s) encode_param(w, p);
}

// Read a single CBOR value for a keymaster tag of the given type.
bool read_tag_value(CborReader& r, KeyParam& p) {
    if (is_bytes_type(p.tag)) return r.bytes(p.blob);
    if (is_bool_type(p.tag)) {                          // CBOR simple true/false
        uint8_t m; uint64_t v; if (!r.head(m, v)) return false;
        p.integer = 1; return m == 7;
    }
    int64_t v; if (!r.i(v)) return false; p.integer = (uint64_t)v; return true;
}

// Parse a bare AuthSet sub-map {tag:val...} (characteristics; no 22 count key).
bool parse_authset_plain(CborReader& r, AuthSet& out) {
    uint64_t np; if (!r.map(np)) return false;
    for (uint64_t k = 0; k < np; ++k) {
        int64_t key; if (!r.i(key)) return false;
        KeyParam p; p.tag = (uint32_t)key;
        if (!read_tag_value(r, p)) return false;
        out.push_back(std::move(p));
    }
    return true;
}

// A decoded request map: keymaster tag params plus the small-int field-id members.
struct ReqMap {
    AuthSet params;
    std::unordered_map<int, uint64_t> ints;   // 22,24,34,37
    std::unordered_map<int, std::vector<uint8_t>> blobs;  // 23,25,29,30,35,36,38,41,42,43,44
    int bool46 = -1;                           // deviceLocked
    bool has(int fid) const { return ints.count(fid) || blobs.count(fid); }
};

// field-id value kinds
bool fid_is_uint(int f) { return f == 22 || f == 24 || f == 34 || f == 37; }
bool fid_is_bool(int f) { return f == 46; }

bool parse_req(const uint8_t* data, size_t len, ReqMap& out) {
    CborReader r(data, len);
    uint64_t np; if (!r.map(np)) return false;
    for (uint64_t k = 0; k < np; ++k) {
        int64_t key; if (!r.i(key)) return false;
        uint32_t tag = (uint32_t)key;
        if ((tag & TYPE_MASK) != 0) {                   // keymaster tag param
            KeyParam p; p.tag = tag;
            if (!read_tag_value(r, p)) return false;
            out.params.push_back(std::move(p));
        } else {                                        // small-int field id
            int fid = (int)tag;
            if (fid == 22) { uint64_t v; if (!r.u(v)) return false; out.ints[22] = v; }
            else if (fid_is_uint(fid)) { uint64_t v; if (!r.u(v)) return false; out.ints[fid] = v; }
            else if (fid_is_bool(fid)) { uint8_t m; uint64_t v; if (!r.head(m, v)) return false;
                out.bool46 = (m == 7 && (v & 1)) ? 1 : 0; }
            else { std::vector<uint8_t> b; if (!r.bytes(b)) return false; out.blobs[fid] = std::move(b); }
        }
    }
    return true;
}

// ---- deterministic synthetic key material + crypto ----------------------
uint64_t g_key_counter = 0x9e3779b97f4a7c15ull;
void derive_bytes(uint8_t* out, size_t n, uint64_t salt) {
    size_t off = 0; uint64_t ctr = salt;
    while (off < n) {
        uint8_t seed[24];
        std::memcpy(seed, &ctr, 8);
        uint64_t o = off; std::memcpy(seed + 8, &o, 8);
        std::memcpy(seed + 16, "hwkmderv", 8);
        uint8_t d[32]; hw::crypto::sha256(seed, sizeof seed, d);
        size_t take = (n - off < 32) ? (n - off) : 32;
        std::memcpy(out + off, d, take); off += take; ctr = ctr * 6364136223846793005ull + 1;
    }
}
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (klen > 64) hw::crypto::sha256(key, klen, k); else std::memcpy(k, key, klen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    std::vector<uint8_t> in(ipad, ipad + 64); in.insert(in.end(), msg, msg + mlen);
    uint8_t ih[32]; hw::crypto::sha256(in.data(), in.size(), ih);
    uint8_t ob[96]; std::memcpy(ob, opad, 64); std::memcpy(ob + 64, ih, 32);
    hw::crypto::sha256(ob, 96, out);
}
// Synthetic reversible stream cipher (keystream = SHA256(key||nonce||ctr) XOR). Its own
// inverse, so encrypt/decrypt round-trip within the keymaster boundary. NOT real AES.
void synth_crypt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce,
                 const uint8_t* in, size_t n, uint8_t* out) {
    size_t off = 0; uint32_t ctr = 0;
    while (off < n) {
        std::vector<uint8_t> s(key); s.insert(s.end(), nonce.begin(), nonce.end());
        for (int i = 0; i < 4; ++i) s.push_back((uint8_t)(ctr >> (8 * i)));
        uint8_t d[32]; hw::crypto::sha256(s.data(), s.size(), d);
        size_t take = (n - off < 32) ? (n - off) : 32;
        for (size_t i = 0; i < take; ++i) out[off + i] = in[off + i] ^ d[i];
        off += take; ctr++;
    }
}
void synth_tag(const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce,
               const std::vector<uint8_t>& aad, const uint8_t* ct, size_t clen, uint8_t tag[16]) {
    std::vector<uint8_t> m(nonce); m.insert(m.end(), aad.begin(), aad.end());
    m.insert(m.end(), ct, ct + clen);
    uint8_t full[32]; hmac_sha256(key.data(), key.size(), m.data(), m.size(), full);
    std::memcpy(tag, full, 16);
}

// ---- key blob (DKMK array) ----------------------------------------------
constexpr uint64_t KM_BLOB_MAGIC = 0x4b4d4b44ull;   // "DKMK"

std::vector<uint8_t> build_keyblob(const std::vector<uint8_t>& material,
                                   const AuthSet& hw, const AuthSet& sw) {
    CborWriter cw; cw.map(2);
    cw.u(1); encode_authset_plain(cw, hw);
    cw.u(0); encode_authset_plain(cw, sw);
    CborWriter kb; kb.array(5);
    kb.u(KM_BLOB_MAGIC);
    kb.u(0); kb.u(0);                     // version, flags (unvalidated)
    kb.bytes(cw.buf);                     // element[3] = characteristics
    kb.bytes(material);                   // element[4] = TA-private raw material
    return kb.buf;
}
bool parse_keyblob(const uint8_t* p, size_t n, AuthSet& hw, AuthSet& sw,
                   std::vector<uint8_t>& material) {
    CborReader r(p, n);
    uint64_t nel; if (!r.array(nel) || nel < 5) return false;
    uint64_t magic; if (!r.u(magic) || magic != KM_BLOB_MAGIC) return false;
    uint64_t tmp; if (!r.u(tmp) || !r.u(tmp)) return false;   // version, flags
    std::vector<uint8_t> chars; if (!r.bytes(chars)) return false;
    if (!r.bytes(material)) return false;
    CborReader cr(chars.data(), chars.size());
    uint64_t np; if (!cr.map(np)) return false;
    for (uint64_t i = 0; i < np; ++i) {
        int64_t lvl; if (!cr.i(lvl)) return false;
        AuthSet s; if (!parse_authset_plain(cr, s)) return false;
        if (lvl == 1) hw = std::move(s); else sw = std::move(s);
    }
    return true;
}

// Build KeyCharacteristics (hw + sw AuthSets) from the requested key params.
// Everything key-defining goes hw-enforced (we present as a TEE); secrets and
// request-only tags are dropped; creation time goes sw-enforced.
void build_characteristics(const AuthSet& req, AuthSet& hw, AuthSet& sw,
                           uint32_t os_ver, uint32_t os_patch, uint64_t origin) {
    for (auto& p : req) {
        if (p.tag == TAG_APPLICATION_ID || p.tag == TAG_APPLICATION_DATA ||
            p.tag == TAG_ROOT_OF_TRUST || p.tag == TAG_NONCE)
            continue;                                  // not part of returned characteristics
        hw.push_back(p);
    }
    KeyParam o; o.tag = TAG_ORIGIN; o.integer = origin; hw.push_back(o);
    KeyParam ov; ov.tag = TAG_OS_VERSION; ov.integer = os_ver; hw.push_back(ov);
    KeyParam op; op.tag = TAG_OS_PATCHLEVEL; op.integer = os_patch; hw.push_back(op);
    KeyParam cd; cd.tag = TAG_CREATION_DATETIME; cd.integer = 1704067200000ull; sw.push_back(cd);
}

// ---- TA state -----------------------------------------------------------
struct Op {
    std::vector<uint8_t> material;
    uint64_t purpose = 0, algorithm = 0, block_mode = 0;
    std::vector<uint8_t> nonce, aad, input;
    uint32_t mac_bytes = 16;
};
struct KmState {
    uint32_t os_version = 0, os_patchlevel = 0;
    uint8_t hmac_key[32]; bool hmac_ready = false;
    std::unordered_map<uint64_t, Op> ops;
    uint64_t next_op = 0x0100000000000001ull;
};
KmState g;
void ensure_hmac() { if (!g.hmac_ready) { derive_bytes(g.hmac_key, 32, 0xA5A5A5A5ull); g.hmac_ready = true; } }

// ---- command ids (low bits, after stripping the 0x2000 CBOR flag) -------
constexpr uint32_t C_ADD_RNG = 0x107, C_GENERATE = 0x108, C_GET_CHARS = 0x109,
                   C_IMPORT = 0x10b, C_EXPORT = 0x10c, C_DELETE = 0x10d, C_DELETE_ALL = 0x10e,
                   C_BEGIN = 0x10f, C_UPDATE = 0x111, C_FINISH = 0x112, C_ABORT = 0x113,
                   C_UPGRADE = 0x114, C_ATTEST = 0x115, C_CONFIGURE = 0x116,
                   C_GET_HMAC_PARAMS = 0x20e, C_COMPUTE_HMAC = 0x20f, C_DEVICE_LOCKED = 0x210,
                   C_EARLY_BOOT = 0x212;

int g_log = 0;
const char* cmd_name(uint32_t id) {
    switch (id) {
    case C_CONFIGURE: return "configure"; case C_GENERATE: return "generateKey";
    case C_GET_CHARS: return "getKeyCharacteristics"; case C_IMPORT: return "importKey";
    case C_EXPORT: return "exportKey"; case C_DELETE: return "deleteKey";
    case C_DELETE_ALL: return "deleteAllKeys"; case C_BEGIN: return "begin";
    case C_UPDATE: return "update"; case C_FINISH: return "finish"; case C_ABORT: return "abort";
    case C_UPGRADE: return "upgradeKey"; case C_ATTEST: return "attestKey";
    case C_ADD_RNG: return "addRngEntropy"; case C_GET_HMAC_PARAMS: return "getHmacSharingParameters";
    case C_COMPUTE_HMAC: return "computeSharedHmac"; case C_DEVICE_LOCKED: return "deviceLocked";
    case C_EARLY_BOOT: return "earlyBootEnded";
    default: return "?";
    }
}

} // namespace

bool keymaster_ta_handle(uint32_t cmd, const uint8_t* payload, size_t len,
                         std::vector<uint8_t>& rsp, bool log) {
    uint32_t id = cmd & ~0x2000u;
    bool lg = log && g_log < 6000;
    if (lg) { g_log++; std::printf("[KM] %s (id=%#x) payload=%zuB\n", cmd_name(id), id, len); }

    int32_t status = KM_OK;
    CborWriter body;                                   // the CBOR response body (after ENV)

    switch (id) {

    case C_CONFIGURE: {
        ReqMap q; parse_req(payload, len, q);
        if (auto* p = find(q.params, TAG_OS_VERSION)) g.os_version = (uint32_t)p->integer;
        if (auto* p = find(q.params, TAG_OS_PATCHLEVEL)) g.os_patchlevel = (uint32_t)p->integer;
        if (lg) std::printf("[KM]   configure os_version=%u os_patchlevel=%u\n", g.os_version, g.os_patchlevel);
        break;                                         // status-only
    }

    case C_ADD_RNG:
    case C_DELETE:
    case C_DELETE_ALL:
    case C_DEVICE_LOCKED:
    case C_EARLY_BOOT:
        break;                                         // status-only ack

    case C_GENERATE:
    case C_IMPORT: {
        ReqMap q; parse_req(payload, len, q);
        uint64_t alg = 0; if (auto* p = find(q.params, TAG_ALGORITHM)) alg = p->integer;
        uint32_t key_bits = 256; if (auto* p = find(q.params, TAG_KEY_SIZE)) key_bits = (uint32_t)p->integer;
        std::vector<uint8_t> material;
        uint64_t origin = 0;                           // GENERATED
        if (id == C_IMPORT && q.blobs.count(25)) { material = q.blobs[25]; origin = 2; /*IMPORTED*/ }
        else {
            size_t nbytes = (alg == ALG_AES || alg == ALG_HMAC || alg == ALG_TRIPLE_DES)
                            ? (key_bits + 7) / 8 : 32;
            if (nbytes == 0 || nbytes > 512) nbytes = 32;
            material.resize(nbytes);
            derive_bytes(material.data(), material.size(), g_key_counter++);
        }
        AuthSet hw, sw; build_characteristics(q.params, hw, sw, g.os_version, g.os_patchlevel, origin);
        std::vector<uint8_t> blob = build_keyblob(material, hw, sw);
        body.map(1); body.u(23); body.bytes(blob);     // { 23 : keyblob }
        if (lg) std::printf("[KM]   %s alg=%llu key_bits=%u material=%zuB blob=%zuB\n",
                            cmd_name(id), (unsigned long long)alg, key_bits, material.size(), blob.size());
        break;
    }

    case C_GET_CHARS: {
        ReqMap q; parse_req(payload, len, q);
        AuthSet hw, sw; std::vector<uint8_t> material;
        if (!q.blobs.count(23) || !parse_keyblob(q.blobs[23].data(), q.blobs[23].size(), hw, sw, material)) {
            status = KM_ERR_INVALID_KEY_BLOB; break;
        }
        body.map(2);
        body.u(1); encode_authset_plain(body, hw);
        body.u(0); encode_authset_plain(body, sw);
        break;
    }

    case C_EXPORT: {                                   // return raw material (compat)
        ReqMap q; parse_req(payload, len, q);
        AuthSet hw, sw; std::vector<uint8_t> material;
        if (!q.blobs.count(23) || !parse_keyblob(q.blobs[23].data(), q.blobs[23].size(), hw, sw, material)) {
            status = KM_ERR_INVALID_KEY_BLOB; break;
        }
        body.map(1); body.u(36); body.bytes(material); // { 36 : material }
        break;
    }

    case C_UPGRADE: {                                  // re-emit the blob with current OS levels
        ReqMap q; parse_req(payload, len, q);
        AuthSet hw, sw; std::vector<uint8_t> material;
        if (!q.blobs.count(23) || !parse_keyblob(q.blobs[23].data(), q.blobs[23].size(), hw, sw, material)) {
            status = KM_ERR_INVALID_KEY_BLOB; break;
        }
        for (auto& p : hw) { if (p.tag == TAG_OS_VERSION) p.integer = g.os_version;
                             if (p.tag == TAG_OS_PATCHLEVEL) p.integer = g.os_patchlevel; }
        std::vector<uint8_t> blob = build_keyblob(material, hw, sw);
        body.map(1); body.u(23); body.bytes(blob);
        break;
    }

    case C_BEGIN: {
        ReqMap q; parse_req(payload, len, q);
        AuthSet hw, sw; std::vector<uint8_t> material;
        if (!q.blobs.count(23) || !parse_keyblob(q.blobs[23].data(), q.blobs[23].size(), hw, sw, material)) {
            status = KM_ERR_INVALID_KEY_BLOB; break;
        }
        Op op; op.material = material;
        if (auto* p = find(q.params, TAG_PURPOSE)) op.purpose = p->integer;
        if (auto* p = find(hw, TAG_ALGORITHM)) op.algorithm = p->integer;
        if (auto* p = find(hw, TAG_BLOCK_MODE)) op.block_mode = p->integer;
        if (auto* p = find(q.params, TAG_MAC_LENGTH)) op.mac_bytes = (uint32_t)(p->integer / 8);
        AuthSet out_params;
        if (auto* p = find(q.params, TAG_NONCE)) op.nonce = p->blob;   // caller-supplied (decrypt)
        else if (op.purpose == PURP_ENCRYPT && (op.block_mode == BM_GCM || op.block_mode == BM_CBC ||
                                                op.block_mode == BM_CTR)) {
            size_t nlen = (op.block_mode == BM_GCM) ? 12 : 16;
            op.nonce.resize(nlen); derive_bytes(op.nonce.data(), nlen, g_key_counter++);
            KeyParam np; np.tag = TAG_NONCE; np.blob = op.nonce; out_params.push_back(np);  // return IV
        }
        uint64_t h = g.next_op++;
        g.ops[h] = std::move(op);
        encode_params(body, out_params);
        body.u(34); body.u(h);                         // { 22:M, [nonce], 34:opHandle }
        if (lg) std::printf("[KM]   begin purpose=%llu alg=%llu bm=%llu handle=%#llx nonce=%zuB\n",
                            (unsigned long long)g.ops[h].purpose, (unsigned long long)g.ops[h].algorithm,
                            (unsigned long long)g.ops[h].block_mode, (unsigned long long)h,
                            g.ops[h].nonce.size());
        break;
    }

    case C_UPDATE: {
        ReqMap q; parse_req(payload, len, q);
        uint64_t h = q.ints.count(34) ? q.ints[34] : 0;
        auto it = g.ops.find(h);
        if (it == g.ops.end()) { status = KM_ERR_INVALID_OPERATION_HANDLE; break; }
        if (auto* p = find(q.params, TAG_ASSOCIATED_DATA)) it->second.aad.insert(
            it->second.aad.end(), p->blob.begin(), p->blob.end());
        uint32_t consumed = 0;
        if (q.blobs.count(35)) { auto& in = q.blobs[35];
            it->second.input.insert(it->second.input.end(), in.begin(), in.end());
            consumed = (uint32_t)in.size(); }
        // Buffer input; produce all output at finish. Empty output here.
        encode_params(body, AuthSet{});                // 22:0
        body.u(36); body.bytes(nullptr, 0);            // 36: (empty output)
        body.u(37); body.u(consumed);                  // 37: inputConsumed
        break;
    }

    case C_FINISH: {
        ReqMap q; parse_req(payload, len, q);
        uint64_t h = q.ints.count(34) ? q.ints[34] : 0;
        auto it = g.ops.find(h);
        if (it == g.ops.end()) { status = KM_ERR_INVALID_OPERATION_HANDLE; break; }
        Op& op = it->second;
        if (auto* p = find(q.params, TAG_ASSOCIATED_DATA)) op.aad.insert(op.aad.end(), p->blob.begin(), p->blob.end());
        if (q.blobs.count(35)) { auto& in = q.blobs[35]; op.input.insert(op.input.end(), in.begin(), in.end()); }
        std::vector<uint8_t> output;
        bool gcm = (op.block_mode == BM_GCM);
        if (op.purpose == PURP_SIGN || op.algorithm == ALG_HMAC) {
            uint8_t mac[32]; hmac_sha256(op.material.data(), op.material.size(),
                                         op.input.data(), op.input.size(), mac);
            uint32_t mb = op.mac_bytes ? op.mac_bytes : 32; if (mb > 32) mb = 32;
            output.assign(mac, mac + mb);
        } else if (op.purpose == PURP_VERIFY) {
            // signature in field 38; recompute + compare
            uint8_t mac[32]; hmac_sha256(op.material.data(), op.material.size(),
                                         op.input.data(), op.input.size(), mac);
            uint32_t mb = op.mac_bytes ? op.mac_bytes : 32; if (mb > 32) mb = 32;
            std::vector<uint8_t>& sig = q.blobs[38];
            if (sig.size() != mb || std::memcmp(sig.data(), mac, mb) != 0) status = KM_ERR_VERIFICATION_FAILED;
        } else if (op.purpose == PURP_ENCRYPT) {
            output.resize(op.input.size());
            synth_crypt(op.material, op.nonce, op.input.data(), op.input.size(), output.data());
            if (gcm) { uint8_t tag[16]; synth_tag(op.material, op.nonce, op.aad, output.data(), output.size(), tag);
                       output.insert(output.end(), tag, tag + op.mac_bytes); }
        } else if (op.purpose == PURP_DECRYPT) {
            std::vector<uint8_t> ct = op.input;
            std::vector<uint8_t> tag;
            if (gcm) {
                if (q.blobs.count(38)) tag = q.blobs[38];
                else if (ct.size() >= op.mac_bytes) { tag.assign(ct.end() - op.mac_bytes, ct.end());
                    ct.resize(ct.size() - op.mac_bytes); }
                uint8_t exp[16]; synth_tag(op.material, op.nonce, op.aad, ct.data(), ct.size(), exp);
                if (tag.size() != op.mac_bytes || std::memcmp(tag.data(), exp, op.mac_bytes) != 0)
                    status = KM_ERR_VERIFICATION_FAILED;
            }
            if (status == KM_OK) { output.resize(ct.size());
                synth_crypt(op.material, op.nonce, ct.data(), ct.size(), output.data()); }
        }
        g.ops.erase(it);
        if (status == KM_OK) { encode_params(body, AuthSet{}); body.u(36); body.bytes(output); }
        if (lg) std::printf("[KM]   finish handle=%#llx purpose=%llu out=%zuB status=%d\n",
                            (unsigned long long)h, (unsigned long long)op.purpose, output.size(), status);
        break;
    }

    case C_ABORT: {
        ReqMap q; parse_req(payload, len, q);
        uint64_t h = q.ints.count(34) ? q.ints[34] : 0;
        g.ops.erase(h);
        break;
    }

    case C_GET_HMAC_PARAMS: {
        ensure_hmac();
        std::vector<uint8_t> seed(32, 0), nonce(32);
        derive_bytes(nonce.data(), 32, 0x5EED0001ull); // per-boot but deterministic
        body.map(2); body.u(41); body.bytes(seed); body.u(42); body.bytes(nonce);
        break;
    }

    case C_COMPUTE_HMAC: {
        ReqMap q; parse_req(payload, len, q);
        ensure_hmac();
        uint8_t out[32];
        static const char kLabel[] = "Keymaster HMAC Verification";
        const std::vector<uint8_t>& packed = q.blobs.count(43) ? q.blobs[43] : std::vector<uint8_t>{};
        std::vector<uint8_t> msg((const uint8_t*)kLabel, (const uint8_t*)kLabel + sizeof(kLabel) - 1);
        msg.insert(msg.end(), packed.begin(), packed.end());
        hmac_sha256(g.hmac_key, 32, msg.data(), msg.size(), out);
        body.map(1); body.u(44); body.bytes(out, 32);
        break;
    }

    case C_ATTEST:                                     // attestation not supported (compat)
        status = KM_ERR_UNIMPLEMENTED;
        break;

    default:
        if (lg) std::printf("[KM]   UNHANDLED id=%#x -> status 0 (empty)\n", id);
        break;                                         // unknown: status-only OK
    }

    // Envelope: [int32 status][u32 cbor_len][cbor body]
    uint32_t cl = (status == KM_OK) ? (uint32_t)body.buf.size() : 0;
    rsp.resize(8 + cl);
    std::memcpy(rsp.data(), &status, 4);
    std::memcpy(rsp.data() + 4, &cl, 4);
    if (cl) std::memcpy(rsp.data() + 8, body.buf.data(), cl);
    if (lg && status != KM_OK) std::printf("[KM]   -> status=%d (error)\n", status);
    return true;
}

} // namespace km
