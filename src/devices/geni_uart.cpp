#include "devices/geni_uart.h"
#include <cstdio>

namespace hw::dev {

namespace {
// SE register offsets (qcom-geni-se.h).
constexpr uint64_t SE_GENI_M_CMD0         = 0x600;
constexpr uint64_t SE_GENI_M_CMD_CTRL_REG = 0x604;
constexpr uint64_t SE_GENI_M_IRQ_STATUS   = 0x610;
constexpr uint64_t SE_GENI_M_IRQ_EN       = 0x614;
constexpr uint64_t SE_GENI_M_IRQ_CLEAR    = 0x618;
constexpr uint64_t SE_GENI_S_IRQ_STATUS   = 0x640;
constexpr uint64_t SE_GENI_S_IRQ_CLEAR    = 0x648;
constexpr uint64_t SE_GENI_TX_FIFOn       = 0x700;
constexpr uint64_t SE_GENI_RX_FIFOn       = 0x780;
constexpr uint64_t SE_GENI_TX_FIFO_STATUS = 0x800;
constexpr uint64_t SE_GENI_RX_FIFO_STATUS = 0x804;
constexpr uint64_t SE_GENI_TX_WATERMARK   = 0x80c;

// M_IRQ_STATUS bits.
constexpr uint32_t M_CMD_DONE_EN          = 1u << 0;
constexpr uint32_t M_TX_FIFO_WATERMARK_EN = 1u << 30;

// M_CMD0 encoding.
constexpr uint32_t M_OPCODE_SHIFT = 27;
constexpr uint32_t UART_START_TX   = 0x1;
} // namespace

void GeniUart::push_byte(uint8_t b) {
    if (b == '\n') { flush_line(); return; }
    if (b == '\r') return;
    line_.push_back((char)b);
    if (line_.size() >= 512) flush_line();
}

void GeniUart::flush_line() {
    std::fprintf(stdout, "  \x1b[2m[guest]\x1b[0m %s\n", line_.c_str());
    std::fflush(stdout);
    line_.clear();
}

uint64_t GeniUart::read(uint64_t offset, unsigned) {
    switch (offset) {
        case SE_GENI_M_IRQ_STATUS: {
            uint32_t s = m_irq_status_;
            // While a TX command is active, keep the watermark asserted so the
            // driver keeps feeding the FIFO; raise CMD_DONE once drained.
            if (tx_bytes_remaining_ > 0) s |= M_TX_FIFO_WATERMARK_EN;
            else s |= M_CMD_DONE_EN;
            return s;
        }
        case SE_GENI_M_IRQ_EN:       return m_irq_en_;
        case SE_GENI_S_IRQ_STATUS:   return 0;
        case SE_GENI_TX_FIFO_STATUS: return 0;                 // FIFO empty -> room to write
        case SE_GENI_RX_FIFO_STATUS: return 0;                 // nothing to receive
        default:                     return 0;
    }
}

void GeniUart::write(uint64_t offset, uint64_t value, unsigned) {
    uint32_t v = (uint32_t)value;
    if (offset >= SE_GENI_TX_FIFOn && offset < SE_GENI_TX_FIFOn + 0x40) {
        // A 32-bit word of up to 4 characters, LSB first.
        for (int i = 0; i < 4 && tx_bytes_remaining_ > 0; ++i) {
            push_byte((uint8_t)(v >> (8 * i)));
            tx_bytes_remaining_--;
        }
        return;
    }
    switch (offset) {
        case SE_GENI_M_CMD0: {
            m_cmd0_ = v;
            uint32_t opcode = (v >> M_OPCODE_SHIFT) & 0x1f;
            if (opcode == UART_START_TX) {
                tx_bytes_remaining_ = v & 0x00ffffff;   // command holds the byte count
                m_irq_status_ |= M_TX_FIFO_WATERMARK_EN;
            }
            break;
        }
        case SE_GENI_M_IRQ_CLEAR:
            m_irq_status_ &= ~v;
            break;
        case SE_GENI_S_IRQ_CLEAR:
            break;
        case SE_GENI_M_IRQ_EN:
            m_irq_en_ = v;
            break;
        case SE_GENI_M_CMD_CTRL_REG:
        case SE_GENI_TX_WATERMARK:
        default:
            break;
    }
}

} // namespace hw::dev
