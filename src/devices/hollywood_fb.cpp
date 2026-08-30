#include "devices/hollywood_fb.h"
#include "gui/gui_bridge.h"
#include "common/log.h"
#include <cstring>
#include <algorithm>

namespace hw::dev {

uint64_t HollywoodFb::read(uint64_t offset, unsigned size) {
    if (offset + size > buf_.size()) return 0;
    uint64_t v = 0;
    std::memcpy(&v, buf_.data() + offset, std::min<unsigned>(size, 8));
    return v;
}

void HollywoodFb::write(uint64_t offset, uint64_t value, unsigned size) {
    if (offset == 0x0) {                       // PRESENT
        uint32_t w = (uint32_t)(value & 0xFFFF);
        uint32_t h = (uint32_t)((value >> 16) & 0xFFFF);
        present(w, h);
        return;
    }
    if (offset + size <= buf_.size())
        std::memcpy(buf_.data() + offset, &value, std::min<unsigned>(size, 8));
}

void HollywoodFb::present(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        HW_WARN("hollywood_fb", "present with bad dims {}x{} -- ignored", w, h);
        return;
    }
    size_t px = (size_t)w * h;
    if (kDataOff + px * 4 > buf_.size()) {
        HW_WARN("hollywood_fb", "frame {}x{} exceeds fb window -- ignored", w, h);
        return;
    }
    auto& b = hw::gui::bridge();
    {
        std::lock_guard<std::mutex> g(b.fb_mu);
        b.fb.resize(px);
        // Guest writes RGBA8888 bytes (R,G,B,A). Repack into 0xAARRGGBB for the
        // host texture path (BGRA order in memory) so colors are correct.
        const uint8_t* src = buf_.data() + kDataOff;
        for (size_t i = 0; i < px; ++i) {
            uint8_t r = src[i*4+0], gg = src[i*4+1], bl = src[i*4+2], a = src[i*4+3];
            b.fb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | bl;
        }
        b.fb_w = (int)w; b.fb_h = (int)h;
        b.fb_gen.fetch_add(1, std::memory_order_release);
    }
    ++frames_;
    HW_INFO("hollywood_fb", "presented frame #{} {}x{} ({} px) -> GUI", frames_, w, h, px);
}

} // namespace hw::dev
