#include "devices/qcom_ufs.h"
#include "devices/ufs_disk.h"
#include "devices/irq.h"
#include "memory/guest_memory.h"
#include "common/log.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

namespace hw::dev {

// Set HOLLYWOOD_UFS_TRACE=1 to log the UTP command/UIC/IRQ flow (very verbose).
static bool ufs_trace() { static bool t = std::getenv("HOLLYWOOD_UFS_TRACE") != nullptr; return t; }
#define UFS_TRACE(...) do { if (ufs_trace()) HW_WARN("ufs", __VA_ARGS__); } while (0)

namespace {
// UFSHCI register offsets.
enum {
    REG_CAP = 0x00, REG_VER = 0x08, REG_IS = 0x20, REG_IE = 0x24, REG_HCS = 0x30,
    REG_HCE = 0x34, REG_UTRLBA = 0x50, REG_UTRLBAU = 0x54, REG_UTRLDBR = 0x58,
    REG_UTRLCLR = 0x5C, REG_UTRLRSR = 0x60, REG_UTMRLBA = 0x70, REG_UTMRLBAU = 0x74,
    REG_UTMRLDBR = 0x78, REG_UTMRLRSR = 0x80, REG_UICCMD = 0x90, REG_ARG1 = 0x94,
    REG_ARG2 = 0x98, REG_ARG3 = 0x9C,
    REG_UFS_CFG1 = 0xDC, REG_UFS_CFG2 = 0xE0, REG_UFS_HW_VERSION = 0xE4,
};
// HCS bits.
enum { HCS_DP = 0x01, HCS_UTRLRDY = 0x02, HCS_UTMRLRDY = 0x04, HCS_UCRDY = 0x08 };
// IS bits.
enum { IS_UTRCS = 0x1, IS_UIC_PWR = 0x10, IS_UHXS = 0x20, IS_UHES = 0x40,
       IS_UIC_LINKUP = 0x100, IS_UTMRCS = 0x200, IS_UCCS = 0x400 };
// UIC opcodes.
enum { DME_GET = 0x01, DME_SET = 0x02, DME_LINKSTARTUP = 0x16,
       DME_HIBERNATE_ENTER = 0x17, DME_HIBERNATE_EXIT = 0x18 };
// UPIU transaction types.
enum { UPIU_NOP_OUT = 0x00, UPIU_CMD = 0x01, UPIU_QUERY_REQ = 0x16,
       UPIU_NOP_IN = 0x20, UPIU_RESPONSE = 0x21, UPIU_QUERY_RESP = 0x36 };

constexpr uint32_t PA_PWRMODE = 0x2141;   // MIB read from UIC ARG1[31:16]
} // namespace

QcomUfs::QcomUfs(uint64_t base, uint64_t size, uint32_t irq_intid,
                 hw::mem::GuestMemory* ram, std::shared_ptr<UfsDisk> disk)
    : base_(base), size_(size), irq_(irq_intid), ram_(ram), disk_(std::move(disk)) {}
QcomUfs::~QcomUfs() = default;

// ---- DMA helpers (guest physical addresses) --------------------------------
void QcomUfs::dma_read(uint64_t gpa, void* dst, size_t len) {
    if (ram_ && ram_->contains(gpa, len)) std::memcpy(dst, ram_->host_ptr(gpa), len);
    else std::memset(dst, 0, len);
}
void QcomUfs::dma_write(uint64_t gpa, const void* src, size_t len) {
    if (ram_ && ram_->contains(gpa, len)) std::memcpy(ram_->host_ptr(gpa), src, len);
}
uint32_t QcomUfs::rd32(uint64_t gpa) { uint32_t v = 0; dma_read(gpa, &v, 4); return v; }
void QcomUfs::wr32(uint64_t gpa, uint32_t v) { dma_write(gpa, &v, 4); }

void QcomUfs::update_irq() {
    bool level = (is_ & ie_) != 0;
    UFS_TRACE("irq {} (is={:#x} ie={:#x})", level ? "RAISE" : "clear", is_, ie_);
    raise_irq(irq_, level);
}

// ---- register interface -----------------------------------------------------
uint64_t QcomUfs::read(uint64_t off, unsigned) {
    // Crypto capability registers (UFSHCI + Qcom ICE). REG_UFS_CCAP 0x100,
    // REG_UFS_CRYPTOCAP array from 0x104. ufshcd_hba_init_crypto_qti_spec requires
    // CAP bit28 (crypto) or it fails fatally; advertise one AES-256-XTS capability
    // and 32 config slots so the keyslot manager initialises cleanly.
    if (off == 0x100) return 0x40001F01u;              // CCAP: cfg_ptr=0x40, cfg_cnt=31, num_cap=1
    if (off == 0x104) return 0x000F0300u;              // CRYPTOCAP[0]: AES-XTS, 256-bit, sdus 512..4K

    switch (off) {
        case REG_CAP: return 0x1107001Fu;              // nutrs=32, nutmrs=8, 64-bit, crypto(bit28)
        case REG_VER: return 0x00000300u;              // UFSHCI 3.0
        case REG_UFS_HW_VERSION: return 0x40000000u;   // Qcom HW major 4 (SM8250)
        case REG_IS:  return is_;
        case REG_IE:  return ie_;
        case REG_HCS: return hcs_ | (1u << 8);         // UPMCRS = PWR_LOCAL(1)
        case REG_HCE: return hce_;
        case REG_UTRLBA:  return utrlba_;
        case REG_UTRLBAU: return utrlbau_;
        case REG_UTRLDBR: return utrldbr_;
        case REG_UTRLRSR: return utrlrsr_;
        case REG_UTMRLBA:  return utmrlba_;
        case REG_UTMRLBAU: return utmrlbau_;
        case REG_UTMRLDBR: return utmrldbr_;
        case REG_UTMRLRSR: return utmrlrsr_;
        case REG_UICCMD: return uiccmd_;
        case REG_ARG1: return uicarg1_;
        case REG_ARG2: return uicarg2_;
        case REG_ARG3: return uicarg3_;
        case REG_UFS_CFG1: return cfg1_;
        case REG_UFS_CFG2: return cfg2_;
        default: return 0;
    }
}

void QcomUfs::write(uint64_t off, uint64_t value, unsigned) {
    uint32_t v = (uint32_t)value;
    switch (off) {
        case REG_HCE:
            hce_ = v & 1;
            hcs_ = hce_ ? (HCS_UCRDY | HCS_UTRLRDY | HCS_UTMRLRDY) : 0;  // lists ready up front
            return;
        case REG_IE:  ie_ = v; update_irq(); return;
        case REG_IS:  is_ &= ~v; update_irq(); return;          // write-1-to-clear
        case REG_UTRLBA:  utrlba_ = v;  return;
        case REG_UTRLBAU: utrlbau_ = v; return;
        case REG_UTRLRSR: utrlrsr_ = v & 1; if (v & 1) hcs_ |= HCS_UTRLRDY; return;
        case REG_UTMRLBA:  utmrlba_ = v;  return;
        case REG_UTMRLBAU: utmrlbau_ = v; return;
        case REG_UTMRLRSR: utmrlrsr_ = v & 1; if (v & 1) hcs_ |= HCS_UTMRLRDY; return;
        case REG_UTMRLDBR:                                       // task mgmt: complete at once
            if (v) { utmrldbr_ = 0; is_ |= IS_UTMRCS; update_irq(); }
            return;
        case REG_UICCMD: uiccmd_ = v; uic_command(v); return;
        case REG_ARG1: uicarg1_ = v; return;
        case REG_ARG2: uicarg2_ = v; return;
        case REG_ARG3: uicarg3_ = v; return;
        case REG_UTRLDBR: {
            uint32_t newbits = v & ~utrldbr_;
            utrldbr_ |= v;
            process_doorbell(newbits);
            return;
        }
        case REG_UTRLCLR: return;                                // driver clears completed tags
        case REG_UFS_CFG1: cfg1_ = v; return;
        case REG_UFS_CFG2: cfg2_ = v; return;
        default: return;
    }
}

void QcomUfs::uic_command(uint32_t cmd) {
    uint32_t opcode = cmd & 0xFF;
    uicarg2_ = 0;                                    // result: success
    if (opcode == DME_LINKSTARTUP) {
        hcs_ |= HCS_DP;                              // device present after link up
        is_ |= IS_UIC_LINKUP;
    }
    if (opcode == DME_SET && (uicarg1_ >> 16) == PA_PWRMODE)
        is_ |= IS_UIC_PWR;                           // power-mode change completed
    if (opcode == DME_HIBERNATE_ENTER) is_ |= IS_UHES;   // hibern8 enter status
    if (opcode == DME_HIBERNATE_EXIT)  is_ |= IS_UHXS;   // hibern8 exit status
    is_ |= IS_UCCS;                                  // UIC command completion
    UFS_TRACE("uic opcode={:#x} arg1={:#x} -> is={:#x}", opcode, uicarg1_, is_);
    update_irq();
}

// ---- transfer ring ----------------------------------------------------------
void QcomUfs::process_doorbell(uint32_t bits) {
    for (int tag = 0; tag < 32; ++tag) {
        if (!(bits & (1u << tag))) continue;
        run_transfer(tag);
        utrldbr_ &= ~(1u << tag);                    // completion clears the doorbell bit
    }
    is_ |= IS_UTRCS;
    update_irq();
}

void QcomUfs::run_transfer(int tag) {
    uint64_t utrl = ((uint64_t)utrlbau_ << 32) | utrlba_;
    uint64_t utrd = utrl + (uint64_t)tag * 32;

    uint64_t ucd = ((uint64_t)rd32(utrd + 20) << 32) | rd32(utrd + 16);
    uint32_t dw6 = rd32(utrd + 24), dw7 = rd32(utrd + 28);
    uint32_t resp_off = (dw6 >> 16) & 0xFFFF;        // dwords
    uint32_t prdt_off = (dw7 >> 16) & 0xFFFF;        // dwords
    uint32_t prdt_len = dw7 & 0xFFFF;                // entries
    uint64_t cmd_upiu = ucd;
    uint64_t rsp_upiu = ucd + (uint64_t)resp_off * 4;
    uint64_t prdt     = ucd + (uint64_t)prdt_off * 4;

    uint8_t hdr[12];
    dma_read(cmd_upiu, hdr, 12);
    uint8_t type = hdr[0], lun = hdr[2], task_tag = hdr[3];
    UFS_TRACE("xfer tag={} type={:#x} lun={:#x} ucd={:#x} prdt_len={}", tag, type, lun, ucd, prdt_len);

    // Build a success response UPIU header (type set per branch below).
    auto write_resp = [&](uint8_t rtype, uint8_t scsi_status, uint16_t data_seg_len) {
        uint8_t r[32]; std::memset(r, 0, sizeof r);
        r[0] = rtype; r[2] = lun; r[3] = task_tag;
        r[6] = 0;                    // response = TARGET SUCCESS
        r[7] = scsi_status;          // SCSI status (GOOD=0)
        r[10] = (uint8_t)(data_seg_len >> 8); r[11] = (uint8_t)data_seg_len;  // be16
        dma_write(rsp_upiu, r, 32);
    };
    // Copy `len` bytes of `data` into the PRDT-described buffers (data-in).
    auto write_data_in = [&](const uint8_t* data, size_t len) {
        size_t done = 0;
        for (uint32_t i = 0; i < prdt_len && done < len; ++i) {
            uint64_t e = prdt + (uint64_t)i * 16;
            uint64_t addr = ((uint64_t)rd32(e + 4) << 32) | rd32(e + 0);
            uint32_t sz = (rd32(e + 12) & 0x3FFFF) + 1;       // 0-based byte count
            size_t n = std::min((size_t)sz, len - done);
            dma_write(addr, data + done, n);
            done += n;
        }
    };
    // Gather `len` bytes from the PRDT-described buffers (data-out) into `data`.
    auto read_data_out = [&](uint8_t* data, size_t len) {
        size_t done = 0;
        for (uint32_t i = 0; i < prdt_len && done < len; ++i) {
            uint64_t e = prdt + (uint64_t)i * 16;
            uint64_t addr = ((uint64_t)rd32(e + 4) << 32) | rd32(e + 0);
            uint32_t sz = (rd32(e + 12) & 0x3FFFF) + 1;       // 0-based byte count
            size_t n = std::min((size_t)sz, len - done);
            dma_read(addr, data + done, n);
            done += n;
        }
    };

    uint32_t ocs = 0;                                // OCS_SUCCESS

    if (type == UPIU_NOP_OUT) {
        write_resp(UPIU_NOP_IN, 0, 0);
    } else if (type == UPIU_QUERY_REQ) {
        // Transaction-specific fields at cmd_upiu+12.
        uint8_t tsf[16]; dma_read(cmd_upiu + 12, tsf, 16);
        uint8_t qop = tsf[0], idn = tsf[1];
        // Response UPIU = 12B header + 16B TSF + data segment (descriptor, up to
        // 0x40 bytes). Buffer must hold 12+16+64 = 92; size to 128 with margin.
        uint8_t resp[128]; std::memset(resp, 0, sizeof resp);
        resp[0] = UPIU_QUERY_RESP; resp[2] = lun; resp[3] = task_tag;
        std::memcpy(resp + 12, tsf, 16);             // echo TSF
        resp[6] = 0;                                 // query response = success
        uint16_t dseg = 0;
        if (qop == 0x01) {                           // READ DESCRIPTOR
            uint8_t desc[0x40]; std::memset(desc, 0, sizeof desc);
            if (idn == 0x00) {                       // Device Descriptor
                desc[0] = 0x40; desc[1] = 0x00; desc[6] = 1 /*bNumberLU*/; desc[5] = 0;
            } else { desc[0] = 0x10; desc[1] = idn; }
            uint16_t want = ((uint16_t)tsf[6] << 8) | tsf[7];
            if (!want || want > desc[0]) want = desc[0];   // clamp to descriptor length (<=0x40)
            std::memcpy(resp + 32, desc, want);      // data segment at GENERAL_UPIU_REQUEST_SIZE (32)
            dseg = want;
            resp[12 + 6] = (uint8_t)(want >> 8); resp[12 + 7] = (uint8_t)want; // TSF length
        }
        // read/write flag & attribute: value stays 0 (fine for bring-up).
        resp[10] = (uint8_t)(dseg >> 8); resp[11] = (uint8_t)dseg;   // dword_2 data-seg len
        dma_write(rsp_upiu, resp, (size_t)32 + dseg); // header+OSF(20) then data segment
    } else if (type == UPIU_CMD) {
        uint8_t cdb[16]; dma_read(cmd_upiu + 16, cdb, 16);     // header(12)+exp_len(4)
        uint8_t op = cdb[0];
        UFS_TRACE("scsi cdb op={:#x} lun={:#x}", op, lun);
        bool wlun = (lun & 0x80) != 0;
        uint8_t status = 0;
        if (op == 0x00) {                            // TEST UNIT READY
        } else if (op == 0x12) {                     // INQUIRY
            uint8_t inq[96]; std::memset(inq, 0, sizeof inq);
            inq[0] = 0x00;                           // peripheral: direct-access block device
            inq[1] = 0x00; inq[2] = 0x06; inq[3] = 0x02; inq[4] = 91;   // additional length
            std::memcpy(inq + 8,  "HOLLYWD ", 8);
            std::memcpy(inq + 16, "UFS STORAGE     ", 16);
            std::memcpy(inq + 32, "0100", 4);
            write_data_in(inq, 36);
        } else if (op == 0x25) {                     // READ CAPACITY(10)
            uint64_t last = disk_ ? disk_->block_count() - 1 : 0;
            uint32_t l = last > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)last;
            uint32_t bs = disk_ ? disk_->block_size() : 4096;
            uint8_t d[8] = { (uint8_t)(l>>24),(uint8_t)(l>>16),(uint8_t)(l>>8),(uint8_t)l,
                             (uint8_t)(bs>>24),(uint8_t)(bs>>16),(uint8_t)(bs>>8),(uint8_t)bs };
            write_data_in(d, 8);
        } else if (op == 0x9E && (cdb[1] & 0x1F) == 0x10) {   // READ CAPACITY(16)
            uint64_t last = disk_ ? disk_->block_count() - 1 : 0;
            uint32_t bs = disk_ ? disk_->block_size() : 4096;
            uint8_t d[32]; std::memset(d, 0, sizeof d);
            for (int i = 0; i < 8; ++i) d[i] = (uint8_t)(last >> (56 - 8*i));
            for (int i = 0; i < 4; ++i) d[8+i] = (uint8_t)(bs >> (24 - 8*i));
            write_data_in(d, 32);
        } else if (op == 0xA0) {                     // REPORT LUNS -> just LUN0
            uint8_t d[16]; std::memset(d, 0, sizeof d);
            d[3] = 8;                                // list length = 8 (one LUN)
            write_data_in(d, 16);
        } else if (op == 0x5A) {                     // MODE SENSE(10)
            uint8_t d[8]; std::memset(d, 0, sizeof d); d[1] = 6;   // mode data length
            write_data_in(d, 8);
        } else if ((op == 0x28 || op == 0x88) && !wlun) {         // READ(10)/READ(16)
            uint64_t lba; uint32_t cnt;
            if (op == 0x28) { lba = ((uint32_t)cdb[2]<<24)|(cdb[3]<<16)|(cdb[4]<<8)|cdb[5];
                              cnt = ((uint32_t)cdb[7]<<8)|cdb[8]; }
            else { lba = 0; for (int i=0;i<8;++i) lba=(lba<<8)|cdb[2+i];
                   cnt = ((uint32_t)cdb[10]<<24)|(cdb[11]<<16)|(cdb[12]<<8)|cdb[13]; }
            uint32_t bs = disk_ ? disk_->block_size() : 4096;
            uint64_t nblk = disk_ ? disk_->block_count() : 0;
            if (nblk && lba >= nblk) cnt = 0;                 // out-of-range LBA
            else if (nblk && lba + cnt > nblk) cnt = (uint32_t)(nblk - lba);
            if (cnt > 8192) cnt = 8192;                       // cap one transfer (32 MB @ 4K)
            std::vector<uint8_t> buf((size_t)cnt * bs);
            if (disk_ && cnt) disk_->read(lba, cnt, buf.data());
            write_data_in(buf.data(), buf.size());
        } else if ((op == 0x2A || op == 0x8A) && !wlun) {         // WRITE(10)/WRITE(16)
            uint64_t lba; uint32_t cnt;
            if (op == 0x2A) { lba = ((uint32_t)cdb[2]<<24)|(cdb[3]<<16)|(cdb[4]<<8)|cdb[5];
                              cnt = ((uint32_t)cdb[7]<<8)|cdb[8]; }
            else { lba = 0; for (int i=0;i<8;++i) lba=(lba<<8)|cdb[2+i];
                   cnt = ((uint32_t)cdb[10]<<24)|(cdb[11]<<16)|(cdb[12]<<8)|cdb[13]; }
            uint32_t bs = disk_ ? disk_->block_size() : 4096;
            uint64_t nblk = disk_ ? disk_->block_count() : 0;
            if (nblk && lba >= nblk) cnt = 0;                 // out-of-range LBA
            else if (nblk && lba + cnt > nblk) cnt = (uint32_t)(nblk - lba);
            if (cnt > 8192) cnt = 8192;                       // cap one transfer (32 MB @ 4K)
            if (disk_ && cnt) {
                std::vector<uint8_t> buf((size_t)cnt * bs);
                read_data_out(buf.data(), buf.size());
                disk_->write(lba, cnt, buf.data());
            }
        } else if (op == 0x1B || op == 0x03 || op == 0x35 || op == 0x1A) {
            // START STOP UNIT / REQUEST SENSE / SYNC CACHE / MODE SENSE(6): succeed.
        } else {
            status = 0;                              // default: GOOD (permissive)
        }
        write_resp(UPIU_RESPONSE, status, 0);
    } else {
        write_resp(UPIU_RESPONSE, 0, 0);
    }

    uint32_t dw2 = rd32(utrd + 8);
    dw2 = (dw2 & ~0xFu) | (ocs & 0xF);              // OCS in low nibble
    wr32(utrd + 8, dw2);
}

} // namespace hw::dev
