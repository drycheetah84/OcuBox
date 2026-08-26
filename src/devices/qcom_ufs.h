// Qualcomm UFS host controller (UFSHCI 3.0) -- functional model.
//
// Enough of the controller to bring the real Linux ufshcd/ufs-qcom driver all the
// way to block-device discovery: the UFSHCI register file, the HCE reset + UIC
// link-startup handshake, and the UTP transfer-request ring (doorbell -> UTRD ->
// UPIU/CDB -> SCSI -> PRDT DMA -> completion + SPI 265). DMA is physical (the UFS
// node has no IOMMU): UTRLBA/UCD/PRDT addresses are guest physical addresses read
// straight from GuestMemory. Backed by a synthesized GPT disk (LUN 0) whose
// partitions are served from the Quest OTA.
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace hw::mem { class GuestMemory; }

namespace hw::dev {

class UfsDisk;   // backing storage (GPT + OTA partitions)

// Qualcomm Inline Crypto Engine (ICE) at 0x1d90000. ufs-qcom's crypto init reads
// ICE_REGS_VERSION (0x08, major must be >= 3) and polls ICE_REGS_BIST_STATUS
// (0x70, bits 28-31 must be clear); otherwise ufshcd_init fails. We report a v3
// ICE that has passed BIST and expose no keyslots (crypto present but idle).
class QcomIce : public MmioDevice {
public:
    QcomIce(uint64_t base, uint64_t size) : base_(base), size_(size) {}
    const char* name() const override { return "ufs-ice"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }
    uint64_t read(uint64_t off, unsigned) override {
        if (off == 0x08) return 0x03000000u;   // ICE_REGS_VERSION: major=3
        if (off == 0x70) return 0;             // ICE_REGS_BIST_STATUS: passed
        return 0;
    }
    void write(uint64_t, uint64_t, unsigned) override {}
private:
    uint64_t base_, size_;
};

// Qualcomm UFS QMP-v4 PHY at 0x1d87000. The phy-qcom-ufs-qmp-v4 driver calibrates
// the PHY (many register writes) then polls UFS_PHY_PCS_READY_STATUS (PHY_BASE
// 0xC00 + 0x180 = 0xD80) bit0 for up to 1 second. Report PCS ready immediately so
// the driver doesn't busy-spin; other reads return last-written (calibration).
class QcomUfsPhy : public MmioDevice {
public:
    QcomUfsPhy(uint64_t base, uint64_t size) : base_(base), size_(size) {}
    const char* name() const override { return "ufsphy"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }
    uint64_t read(uint64_t off, unsigned) override {
        if (off == 0xD80) return 0x1;          // UFS_PHY_PCS_READY_STATUS: ready
        return 0;
    }
    void write(uint64_t, uint64_t, unsigned) override {}
private:
    uint64_t base_, size_;
};

class QcomUfs : public MmioDevice {
public:
    QcomUfs(uint64_t base, uint64_t size, uint32_t irq_intid,
            hw::mem::GuestMemory* ram, std::shared_ptr<UfsDisk> disk);
    ~QcomUfs() override;

    const char* name() const override { return "ufshc"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t offset, unsigned size) override;
    void write(uint64_t offset, uint64_t value, unsigned size) override;

private:
    void update_irq();                       // raise/lower SPI per (IS & IE)
    void process_doorbell(uint32_t bits);    // run each requested UTP transfer slot
    void run_transfer(int tag);              // one UTRD: parse UPIU/CDB, do SCSI + DMA
    void uic_command(uint32_t cmd);          // UIC link-startup / DME get-set

    // Physical-memory DMA helpers (guest physical addresses).
    void dma_read(uint64_t gpa, void* dst, size_t len);
    void dma_write(uint64_t gpa, const void* src, size_t len);
    uint32_t rd32(uint64_t gpa);
    void     wr32(uint64_t gpa, uint32_t v);

    uint64_t base_, size_;
    uint32_t irq_;
    hw::mem::GuestMemory* ram_;
    std::shared_ptr<UfsDisk> disk_;

    // Register state.
    uint32_t is_ = 0, ie_ = 0, hcs_ = 0, hce_ = 0;
    uint32_t utrlba_ = 0, utrlbau_ = 0, utrldbr_ = 0, utrlrsr_ = 0;
    uint32_t utmrlba_ = 0, utmrlbau_ = 0, utmrldbr_ = 0, utmrlrsr_ = 0;
    uint32_t uiccmd_ = 0, uicarg1_ = 0, uicarg2_ = 0, uicarg3_ = 0;
    uint32_t cfg1_ = 0, cfg2_ = 0;           // Qcom vendor regs (R/W)
};

} // namespace hw::dev
