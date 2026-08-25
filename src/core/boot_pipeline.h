// The hollywood_emu boot pipeline. Each stage performs a real emulator
// operation against the supplied Quest 2 OTA/kernel and reports OK/FAIL; on
// failure (or reaching the current implementation frontier) it prints detailed,
// actionable diagnostics about the prepared machine state.
#pragma once
#include "core/emulator.h"
#include <functional>
#include <string>
#include <vector>

namespace hw::core {

class BootPipeline {
public:
    explicit BootPipeline(Emulator& emu) : emu_(emu) {}
    // Returns process exit code (0 = pipeline reached its frontier cleanly).
    int run();

private:
    // Runs one stage; `fn` returns a short detail string or throws on failure.
    bool stage(const std::string& label, const std::function<std::string()>& fn);
    void print_header();
    void print_diagnostics(const std::string& failed_stage);
    void print_arm64_exception(const cpu::RunResult& rr);
    void walk_page_table(uint64_t va);   // software AArch64 stage-1 walk (VA39/4KB)
    void dump_kmsg();                    // recover the kernel printk log from guest RAM
    bool dump_kmsg_symbols();            // reliable index-based printk ring walk (uses fixed symbol VAs)
    void list_dt();                      // print every DT node path + compatible (device survey)
    void apply_dtb_profile(Bytes& dtb);  // minimal profile: status="disabled" the configured nodes
    void dump_dt();                      // print the timer + GIC device-tree nodes
    void patch_dtb_memory(Bytes& dtb);   // emulate the bootloader: fill /memory size

    uint64_t normalize_addr(uint64_t addr) const;

    Emulator& emu_;
    std::string last_ok_;
    bool failed_ = false;
};

} // namespace hw::core
