#include "platform/qcom_cmddb.h"
#include <cstring>
#include <string>
#include <vector>

namespace hw::platform {

namespace {
// cmd-db on-memory layout (drivers/soc/qcom/cmd-db.c):
//   struct cmd_db_header { u32 version; u8 magic[4]; rsc_hdr header[8];
//                          u32 checksum; u32 reserved; u8 data[]; }  (0x90 fixed)
//   struct rsc_hdr  { u16 slv_id; u16 header_offset; u16 data_offset; u16 cnt;
//                     u16 version; u16 reserved[3]; }                (16 bytes)
//   struct entry_header { u8 id[8]; u32 priority[2]; u32 addr; u16 len; u16 offset; } (24 bytes)
// header_offset/data_offset are relative to data[] (blob + 0x90); an entry's aux
// data lives at data + rsc.data_offset + entry.offset, length entry.len.
constexpr uint8_t  MAGIC[4]   = { 0xdb, 0x30, 0x03, 0x0c };
constexpr size_t   HDR_SIZE   = 0x90;
constexpr size_t   ENTRY_SIZE = 24;

// Slave-id (addr>>16 & 7), from include/soc/qcom/cmd-db.h.
enum Slave { ARC = 3, VRM = 4, BCM = 5 };

struct Res { const char* id; int slave; };

// Every RPMh resource the UFS clock/regulator/bus bring-up looks up (from the
// Quest kernel source). ARC carries a 16-entry level table; BCM carries an 8-byte
// bcm_db aux record; VRM carries no aux.
const Res kResources[] = {
    // ARC (level tables): clk-rpmh + gcc vdd rails.
    { "xo.lvl", ARC }, { "cx.lvl", ARC }, { "mx.lvl", ARC }, { "mmcx.lvl", ARC },
    // VRM clocks (clk-rpmh mandatory) + UFS/PHY PMIC supplies.
    { "lnbclka1", VRM }, { "lnbclka2", VRM }, { "lnbclka3", VRM },
    { "rfclka1", VRM }, { "rfclka3", VRM },
    { "ldoa17", VRM }, { "ldoa6", VRM }, { "smpa4", VRM }, { "ldoa5", VRM }, { "ldoa9", VRM },
    // BCM (bus clock managers) -- required by the msm_bus fabric the UFS driver
    // registers a client with (ufs_qcom_bus_register). The whole fabric probe
    // fails if any BCM's cmd-db info is missing.
    { "ACV", BCM }, { "ALC", BCM }, { "MC0", BCM }, { "SH0", BCM }, { "MM0", BCM },
    { "CE0", BCM }, { "IP0", BCM }, { "MM1", BCM }, { "SH2", BCM }, { "MM2", BCM },
    { "QUP0", BCM }, { "SH3", BCM }, { "MM3", BCM }, { "SH4", BCM },
    { "SN0", BCM }, { "CO0", BCM }, { "CN0", BCM }, { "SN1", BCM }, { "SN2", BCM },
    { "CO2", BCM }, { "SN3", BCM }, { "SN4", BCM }, { "SN5", BCM }, { "SN6", BCM },
    { "SN7", BCM }, { "SN8", BCM }, { "SN9", BCM }, { "SN11", BCM }, { "SN12", BCM },
};

// ARC level table (monotonic non-zero corners; exact values are moot in standalone).
const uint16_t kArcLevels[16] = {
    16, 56, 80, 116, 128, 148, 172, 192, 196, 220, 256, 288, 336, 384, 416, 452,
};
constexpr size_t ARC_AUX_LEN = sizeof(kArcLevels);   // 32 bytes
constexpr size_t BCM_AUX_LEN = 8;                    // struct bcm_db {u32 unit; u16 width; u8 clk_domain; u8 rsvd;}

void wr16(Bytes& b, size_t o, uint16_t v) { b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8); }
void wr32(Bytes& b, size_t o, uint32_t v) {
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8); b[o+2] = (uint8_t)(v >> 16); b[o+3] = (uint8_t)(v >> 24);
}
size_t aux_len_of(int slave) { return slave == ARC ? ARC_AUX_LEN : slave == BCM ? BCM_AUX_LEN : 0; }
} // namespace

Bytes build_cmd_db() {
    // Group resources by slave, preserving order within each group.
    std::vector<const Res*> groups[6];   // index by slave id (3,4,5)
    for (const auto& r : kResources) groups[r.slave].push_back(&r);
    const int slaves[3] = { ARC, VRM, BCM };

    // data[] layout: all entry_headers (grouped by slave), then all aux blobs.
    size_t total_entries = 0;
    for (int s : slaves) total_entries += groups[s].size();
    const size_t entries_region = total_entries * ENTRY_SIZE;

    // Per-slave entry-region and aux-region base offsets (relative to data[]).
    size_t entry_off[6] = {0}, aux_base[6] = {0};
    size_t eo = 0, ao = entries_region;
    for (int s : slaves) { entry_off[s] = eo; eo += groups[s].size() * ENTRY_SIZE; }
    for (int s : slaves) { aux_base[s] = ao; ao += groups[s].size() * aux_len_of(s); }
    const size_t data_len = ao;

    Bytes b(HDR_SIZE + data_len, 0);
    wr32(b, 0x00, 1);                          // version
    std::memcpy(b.data() + 0x04, MAGIC, 4);    // magic
    wr32(b, 0x8C, 0x1);                         // reserved: STANDALONE bit

    // rsc_hdr[] : one per slave, then a zero terminator.
    size_t hi = 0;
    for (int s : slaves) {
        size_t h = 0x08 + hi * 16;
        wr16(b, h + 0, (uint16_t)s);                     // slv_id
        wr16(b, h + 2, (uint16_t)entry_off[s]);          // header_offset (entries)
        wr16(b, h + 4, (uint16_t)aux_base[s]);           // data_offset   (aux base)
        wr16(b, h + 6, (uint16_t)groups[s].size());      // cnt
        wr16(b, h + 8, 1);                               // version
        hi++;
    }
    // rsc_hdr[hi] left zero -> terminates the scan.

    const size_t data = HDR_SIZE;
    for (int s : slaves) {
        size_t aux_len = aux_len_of(s);
        for (size_t i = 0; i < groups[s].size(); ++i) {
            const Res* r = groups[s][i];
            size_t eoff = data + entry_off[s] + i * ENTRY_SIZE;
            std::memset(b.data() + eoff, 0, ENTRY_SIZE);
            std::memcpy(b.data() + eoff, r->id, std::strlen(r->id));   // id[8]
            wr32(b, eoff + 16, ((uint32_t)s << 16) | (uint32_t)(0x100 + i));  // addr (slave in bits[18:16])
            wr16(b, eoff + 20, (uint16_t)aux_len);                    // len
            wr16(b, eoff + 22, (uint16_t)(i * aux_len));              // offset (rel data_offset)
            // aux payload
            size_t aoff = data + aux_base[s] + i * aux_len;
            if (s == ARC) {
                for (size_t k = 0; k < 16; ++k) wr16(b, aoff + k * 2, kArcLevels[k]);
            } else if (s == BCM) {
                wr32(b, aoff + 0, 1000);   // bcm_db.unit (non-zero: avoids div-by-zero in BW calc)
                wr16(b, aoff + 4, 8);      // width
                b[aoff + 6] = 0;           // clk_domain
                b[aoff + 7] = 0;           // reserved
            }
        }
    }
    return b;
}

} // namespace hw::platform
