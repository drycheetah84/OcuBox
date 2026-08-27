// The hollywood_emu boot pipeline. Each stage performs a real emulator
// operation against the supplied Quest 2 OTA/kernel and reports OK/FAIL; on
// failure (or reaching the current implementation frontier) it prints detailed,
// actionable diagnostics about the prepared machine state.
#pragma once
#include "core/emulator.h"
#include <functional>
#include <string>
#include <vector>

namespace hw::ota { class ZipReader; struct Payload; }

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
    void dump_memory_map();              // print /memory + /reserved-memory vs our image placement, flag overlaps
    // Find a 2MB-aligned physical base with `total_size` bytes of RAM that does NOT
    // overlap any fixed (reg-based) /reserved-memory region -- so the kernel image,
    // DTB and initramfs land in genuine usable RAM, not a no-map firmware hole.
    uint64_t find_free_load_base(uint64_t total_size, uint64_t align);

    uint64_t normalize_addr(uint64_t addr) const;

    // Phase 11: cold-boot the real Qualcomm secure monitor (tz) at EL3 in the full
    // machine. Loads the `tz` ELF into its secure carveouts, starts at its entry
    // with EL3h + boot params, and reports where it stops.
    int run_tz(ota::ZipReader& zip, ota::Payload& payload);

    Emulator& emu_;
    std::string last_ok_;
    bool failed_ = false;
};

} // namespace hw::core
