// Top-level emulator state: configuration plus the live machine (RAM, device
// bus, CPU state and execution backend) and the artifacts loaded during boot,
// which the diagnostics reporter reads back.
#pragma once
#include "boot/boot_image.h"
#include "boot/dtb.h"
#include "cpu/cpu_state.h"
#include "devices/device_bus.h"
#include "memory/guest_memory.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hw::core {

struct EmuConfig {
    std::string ota_zip;
    std::string kernel_zip;              // optional: kernel source archive (reference)
    std::string firmware_dir = "firmware";
    std::string config_dir   = "configs";
    uint64_t ram_mb = 2048;
    uint64_t max_instructions = 200000000ull;
    bool verbose = false;
    bool trace = false;          // per-instruction trace (bounded)
    bool stop_on_mmio = true;    // halt on first unclaimed memory access
    bool log_mmio = false;       // log every device MMIO access
    bool dump_dt = false;        // print timer/GIC device-tree nodes and exit
    bool list_dt = false;        // list every DT node path + compatible and exit
    // Boot profile: "stock" = unmodified Quest 2 DTB (faithful); "minimal" =
    // disable non-essential vendor devices to reach userspace. dtb_disable holds
    // extra node ids (compatible substrings or '/'-paths) to disable on top.
    std::string profile = "stock";
    std::vector<std::string> dtb_disable;
    // A/B slot the (emulated) bootloader selects; appended as androidboot.slot_suffix
    // so Android first-stage init can resolve `slotselect` fstab entries.
    std::string slot_suffix = "_a";
    // Phase 11: cold-boot the real Qualcomm secure monitor (tz) at EL3 within the
    // full machine, instead of booting the Linux kernel directly. Loads the `tz`
    // partition into its secure carveouts and starts execution at its entry point.
    bool tz_boot = false;
    // Graphics bring-up (--gfx): inject the SwiftShader Vulkan ICD + vktri via a
    // gfxsrc partition + /init.hollywood.rc, disable dm-verity, and attach the
    // synthetic hollywood_fb capture device. Off by default (preserves the clean boot).
    bool gfx_inject = false;
};

class Emulator {
public:
    explicit Emulator(EmuConfig cfg) : config(std::move(cfg)) {}

    EmuConfig config;

    // Live machine.
    std::unique_ptr<mem::GuestMemory> ram;
    dev::DeviceBus bus;
    cpu::CpuState cpu;
    std::unique_ptr<cpu::CpuBackend> backend;

    // Loaded artifacts / provenance for diagnostics.
    boot::BootImage boot_img;
    std::optional<boot::Fdt> fdt;
    uint64_t dram_base = 0x80000000ull;
    uint64_t ram_size  = 0;
    uint64_t kernel_load = 0;
    uint64_t dtb_load = 0;
    uint64_t ramdisk_load = 0;
    uint64_t ramdisk_size = 0;
    std::string android_build;
    std::string uart_desc = "(none)";
    Bytes dtb_image;                 // DTB actually handed to the kernel (memory-node patched)
    uint64_t dtb_mem_size_old = 0;   // /memory size before we patched it (usually 0)
};

} // namespace hw::core
