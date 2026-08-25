// Qualcomm GENI/QUP UART (Serial Engine) TX console model.
//
// This models enough of the SE register interface used by the Linux
// qcom_geni_serial driver (and its earlycon path) to capture bytes the kernel
// writes to the TX FIFO and mirror them to the host console -- which is how we
// see real Quest 2 kernel log output. RX is stubbed (no host->guest input yet).
//
// Register/bit definitions come from the kernel's drivers/tty/serial/
// qcom_geni_serial.c and include/linux/qcom-geni-se.h.
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <string>

namespace hw::dev {

class GeniUart : public MmioDevice {
public:
    explicit GeniUart(uint64_t base, uint64_t size = 0x4000) : base_(base), size_(size) {}

    const char* name() const override { return "geni_uart"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t offset, unsigned size) override;
    void write(uint64_t offset, uint64_t value, unsigned size) override;

private:
    void push_byte(uint8_t b);
    void flush_line();

    uint64_t base_, size_;

    // Modeled register state.
    uint32_t m_cmd0_ = 0;
    uint32_t m_irq_status_ = 0;
    uint32_t m_irq_en_ = 0;
    uint32_t tx_words_remaining_ = 0;  // words still expected for the active TX command
    uint32_t tx_bytes_remaining_ = 0;

    std::string line_;
};

} // namespace hw::dev
