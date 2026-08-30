// Synthetic framebuffer sink. A guest process (vktri now, a SurfaceFlinger
// present-hook later) mmaps this MMIO region via /dev/mem (it is NOT RAM, so it
// works under CONFIG_STRICT_DEVMEM), writes RGBA8888 pixels into the data window,
// then writes (height<<16 | width) to the PRESENT control register. On present we
// publish the frame into hw::gui::bridge().fb for the host GUI to display.
//
// Layout (relative to base):
//   0x0000  PRESENT (w32): value = (height<<16)|width  -> capture w*h RGBA from data window
//   0x0004  reserved
//   0x1000  data window: RGBA8888 pixels, row-major, tightly packed
#pragma once
#include "devices/device.h"
#include <cstdint>
#include <vector>

namespace hw::dev {

class HollywoodFb : public MmioDevice {
public:
    static constexpr uint64_t kBase = 0x100000000ull;   // first address past 2GB guest RAM
    static constexpr uint64_t kSize = 0x1000000ull;      // 16 MB (fits up to ~2048x2048)
    static constexpr uint64_t kDataOff = 0x1000ull;

    HollywoodFb(uint64_t base = kBase, uint64_t size = kSize)
        : base_(base), size_(size), buf_(size, 0) {}

    const char* name() const override { return "hollywood_fb"; }
    uint64_t base() const override { return base_; }
    uint64_t size() const override { return size_; }
    DevStatus status() const override { return DevStatus::Ok; }

    uint64_t read(uint64_t offset, unsigned size) override;
    void write(uint64_t offset, uint64_t value, unsigned size) override;

private:
    void present(uint32_t w, uint32_t h);
    uint64_t base_, size_;
    std::vector<uint8_t> buf_;
    uint64_t frames_ = 0;
};

} // namespace hw::dev
