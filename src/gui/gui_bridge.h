// Shared state between the emulator thread and the ImGui render thread.
// The emulator's log output is routed here (via hw::Log sink) and a virtual
// display device (added later) publishes the guest framebuffer here.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>

namespace hw::gui {

struct Bridge {
    // --- boot log (written by the log sink on the emulator thread) ---
    std::mutex log_mu;
    std::deque<std::string> log;
    uint64_t total_lines = 0;

    // --- live CPU/boot status (atomics; updated from the emulator) ---
    std::atomic<uint64_t> insns{0};
    std::atomic<uint64_t> pc{0};
    std::atomic<int> stage{0};           // coarse boot stage counter
    std::atomic<bool> emu_running{false};
    std::atomic<bool> emu_done{false};
    std::string status_line;             // guarded by log_mu

    // --- guest framebuffer (RGBA8888), published by the virtual display ---
    std::mutex fb_mu;
    std::vector<uint32_t> fb;
    int fb_w = 0, fb_h = 0;
    std::atomic<uint64_t> fb_gen{0};     // bumped on each new frame

    void push_log(std::string s) {
        std::lock_guard<std::mutex> g(log_mu);
        log.push_back(std::move(s));
        ++total_lines;
        while (log.size() > 6000) log.pop_front();
    }
    void set_status(std::string s) {
        std::lock_guard<std::mutex> g(log_mu);
        status_line = std::move(s);
    }
};

inline Bridge& bridge() { static Bridge b; return b; }

} // namespace hw::gui
