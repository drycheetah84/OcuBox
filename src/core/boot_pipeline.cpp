#include "core/boot_pipeline.h"
#include "common/compress.h"
#include "common/log.h"
#include "devices/geni_uart.h"
#include "devices/gic.h"
#include "devices/stub_device.h"
#include "ota/payload.h"
#include "ota/zip_reader.h"
#include "platform/kona.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace hw::core {

// ---- ANSI helpers ----
namespace {
constexpr const char* GREEN = "\x1b[32m";
constexpr const char* RED   = "\x1b[31m";
constexpr const char* YELL  = "\x1b[33m";
constexpr const char* DIM   = "\x1b[2m";
constexpr const char* BOLD  = "\x1b[1m";
constexpr const char* RST   = "\x1b[0m";

uint64_t round_up(uint64_t x, uint64_t a) { return a ? ((x + a - 1) / a) * a : x; }

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    auto n = (size_t)f.tellg();
    Bytes b(n); f.seekg(0); f.read(reinterpret_cast<char*>(b.data()), (std::streamsize)n);
    return b;
}
void write_file(const fs::path& p, std::span<const uint8_t> data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
}
} // namespace

uint64_t BootPipeline::normalize_addr(uint64_t addr) const {
    if (addr == 0) return 0;
    if (addr >= emu_.dram_base) return addr;   // already absolute
    return emu_.dram_base + addr;              // header stored an offset from base
}

void BootPipeline::print_header() {
    std::printf("\n%s%shollywood_emu%s  %sMeta Quest 2 (hollywood / Snapdragon XR2) boot%s\n",
                BOLD, "", RST, DIM, RST);
    std::printf("%s--------------------------------------------------------------%s\n\n", DIM, RST);
}

bool BootPipeline::stage(const std::string& label, const std::function<std::string()>& fn) {
    if (failed_) return false;
    std::printf("[%s....%s] %s", YELL, RST, label.c_str());
    std::fflush(stdout);
    try {
        std::string detail = fn();
        std::printf("\r[ %sOK%s ] %s", GREEN, RST, label.c_str());
        if (!detail.empty()) std::printf("  %s%s%s", DIM, detail.c_str(), RST);
        std::printf("\n");
        last_ok_ = label;
        return true;
    } catch (const std::exception& e) {
        std::printf("\r[%sFAIL%s] %s\n", RED, RST, label.c_str());
        std::printf("       %s%s%s\n", RED, e.what(), RST);
        failed_ = true;
        print_diagnostics(label);
        return false;
    }
}

int BootPipeline::run() {
    print_header();

    ota::ZipReader* zip = nullptr;
    std::unique_ptr<ota::ZipReader> zip_owner;
    ota::Payload payload;

    // 1) Initializing emulator ------------------------------------------------
    stage("Initializing emulator", [&] {
        Log::set_level(emu_.config.verbose ? LogLevel::Debug : LogLevel::Warn);
        return std::format("host=win-x64  target={}", platform::KonaMap::kName);
    });

    // 2) Loading Quest 2 configuration ---------------------------------------
    stage("Loading Quest 2 configuration", [&] {
        if (emu_.config.ota_zip.empty() || !fs::exists(emu_.config.ota_zip))
            throw std::runtime_error("OTA image not found: " + emu_.config.ota_zip);
        zip_owner = std::make_unique<ota::ZipReader>(emu_.config.ota_zip);
        zip = zip_owner.get();
        // Pull the real Android build fingerprint from the OTA metadata.
        if (auto e = zip->find("META-INF/com/android/metadata")) {
            Bytes meta = zip->read(*e);
            std::string s(reinterpret_cast<const char*>(meta.data()), meta.size());
            auto pos = s.find("post-build=");
            if (pos != std::string::npos) {
                auto end = s.find('\n', pos);
                emu_.android_build = s.substr(pos + 11, end - pos - 11);
            }
        }
        return emu_.android_build.empty() ? std::string("OTA opened")
                                          : ("build " + emu_.android_build);
    });

    // 3) Loading kernel -------------------------------------------------------
    stage("Loading kernel", [&] {
        payload = ota::parse_payload(*zip);
        // Cache boot.img so repeat runs skip the ~1GB payload scan/decompress.
        fs::path cached = fs::path(emu_.config.firmware_dir) / "boot.img";
        Bytes boot;
        if (fs::exists(cached)) {
            boot = read_file(cached);
        } else {
            boot = ota::extract_partition(*zip, payload, "boot");
            write_file(cached, boot);
        }
        emu_.boot_img = boot::parse_boot_image(std::move(boot));
        if (!emu_.boot_img.kernel_is_arm64)
            throw std::runtime_error("kernel is not a recognized ARM64 Image");
        return std::format("ARM64 Image {} KB, image_size={:#x}",
                           emu_.boot_img.kernel.size() / 1024, emu_.boot_img.kernel_image_size);
    });

    // 4) Initializing virtual hardware ---------------------------------------
    stage("Initializing virtual hardware", [&] {
        // Parse the embedded DTB: the device's own description of its hardware.
        if (emu_.boot_img.dtb.empty())
            throw std::runtime_error("boot image carries no embedded DTB");
        emu_.fdt = boot::Fdt::parse(emu_.boot_img.dtb);

        // Memory base from the /memory node (identified by device_type="memory",
        // whose unit name is like "memory@80000000"). Fallback: Kona DRAM base.
        if (const boot::FdtNode* rootn = emu_.fdt->root()) {
            for (const auto& c : rootn->children) {
                const boot::FdtProp* dt = c->prop("device_type");
                if (dt && dt->str() == "memory") {
                    auto regs = c->reg();
                    if (!regs.empty() && regs[0].first != 0) emu_.dram_base = regs[0].first;
                    break;
                }
            }
        }

        // Console UART from the DTB (GENI), fallback to the known QUP base.
        uint64_t uart_base = platform::KonaMap::kQupUart0Base;
        const boot::FdtNode* un = emu_.fdt->find_compatible("qcom,geni-uart");
        if (!un) un = emu_.fdt->find_compatible("qcom,geni-debug-uart");
        if (!un) un = emu_.fdt->find_compatible("geni-uart");
        if (un) { auto r = un->reg(); if (!r.empty()) uart_base = r[0].first; }
        emu_.uart_desc = std::format("geni_uart@{:#x}{}", uart_base, un ? " (dtb)" : " (fallback)");
        emu_.bus.add(std::make_unique<dev::GeniUart>(uart_base));

        // Real GICv3 (distributor + redistributor), addresses/sizes from the DTB.
        uint64_t gicd = platform::KonaMap::kGicDistBase, gicd_sz = platform::KonaMap::kGicDistSize;
        uint64_t gicr = platform::KonaMap::kGicRedistBase, gicr_sz = platform::KonaMap::kGicRedistSize;
        if (const boot::FdtNode* g = emu_.fdt->find_compatible("arm,gic-v3")) {
            auto r = g->reg();
            if (r.size() >= 1) { gicd = r[0].first; gicd_sz = r[0].second; }
            if (r.size() >= 2) { gicr = r[1].first; gicr_sz = r[1].second; }
        }
        emu_.bus.add(std::make_unique<dev::GicV3>(gicd, gicd_sz, gicr, gicr_sz));

        return std::format("dram_base={:#x}, {} MMIO devices", emu_.dram_base, emu_.bus.devices().size());
    });

    if (emu_.config.dump_dt && !failed_) { dump_dt(); return 0; }

    // 5) Loading boot image ---------------------------------------------------
    stage("Loading boot image", [&] {
        std::string r;
        if (!emu_.boot_img.ramdisk.empty()) {
            // Validate the ramdisk really decompresses (gzip cpio).
            try {
                Bytes rd = compress::gzip_decode(emu_.boot_img.ramdisk);
                emu_.ramdisk_size = rd.size();
                r = std::format("ramdisk {} KB (cpio {} KB)",
                                emu_.boot_img.ramdisk.size() / 1024, rd.size() / 1024);
            } catch (...) {
                emu_.ramdisk_size = emu_.boot_img.ramdisk.size();
                r = std::format("ramdisk {} KB (raw)", emu_.boot_img.ramdisk.size() / 1024);
            }
        } else {
            r = "no ramdisk in boot image";
        }
        return r;
    });

    // 6) Initializing memory --------------------------------------------------
    stage("Initializing memory", [&] {
        emu_.ram_size = emu_.config.ram_mb * 1024ull * 1024ull;
        emu_.ram = std::make_unique<mem::GuestMemory>(emu_.dram_base, emu_.ram_size);

        // Kernel load address. Per the ARM64 boot protocol the Image must sit at
        // `text_offset` bytes above a 2MB-aligned base. The boot image's
        // kernel_addr (offset 0x8000) is NOT what the kernel assumes -- using it
        // makes map_kernel() BUG on the alignment. Use the Image header's
        // text_offset from a 2MB-aligned DRAM base.
        uint64_t base2m = round_up(emu_.dram_base, 0x200000);
        emu_.kernel_load = base2m + emu_.boot_img.kernel_text_offset;
        if (!emu_.ram->contains(emu_.kernel_load, emu_.boot_img.kernel.size()))
            emu_.kernel_load = emu_.dram_base + 0x80000;
        emu_.ram->load(emu_.kernel_load, emu_.boot_img.kernel);

        // Place DTB and ramdisk sequentially after the kernel image to guarantee
        // no overlap (bootloader-accurate placement is refined once the CPU runs).
        uint64_t kspan = round_up(std::max<uint64_t>(emu_.boot_img.kernel.size(),
                                                     emu_.boot_img.kernel_image_size), 0x200000);
        uint64_t cursor = emu_.kernel_load + kspan;

        if (!emu_.boot_img.dtb.empty()) {
            // Emulate the bootloader: patch the /memory node with the real RAM size.
            emu_.dtb_image.assign(emu_.boot_img.dtb.begin(), emu_.boot_img.dtb.end());
            patch_dtb_memory(emu_.dtb_image);
            emu_.dtb_load = cursor;
            emu_.ram->load(emu_.dtb_load, emu_.dtb_image);
            cursor = round_up(emu_.dtb_load + emu_.dtb_image.size(), 0x100000);
        }
        if (!emu_.boot_img.ramdisk.empty()) {
            emu_.ramdisk_load = cursor;
            emu_.ram->load(emu_.ramdisk_load, emu_.boot_img.ramdisk);
        }
        return std::format("{} MB @ {:#x}; kernel@{:#x} dtb@{:#x} ramdisk@{:#x}",
                           emu_.config.ram_mb, emu_.dram_base, emu_.kernel_load,
                           emu_.dtb_load, emu_.ramdisk_load);
    });

    // 7) Starting kernel ------------------------------------------------------
    stage("Starting kernel", [&] {
        emu_.cpu.setup_linux_boot(emu_.kernel_load, emu_.dtb_load);
        return std::format("PC={:#x} x0(dtb)={:#x} pstate={:#x}",
                           emu_.cpu.regs.pc, emu_.cpu.regs.x[0], emu_.cpu.regs.pstate);
    });

    // 8) Attach the ARM64 execution backend ----------------------------------
    if (!failed_ && !emu_.backend) {
        // No execution backend was provided -- report the prepared machine.
        std::printf("[%s....%s] %s\n", YELL, RST, "Executing Quest 2 kernel");
        std::printf("       %sno CPU execution backend attached -- machine is prepared and armed "
                    "at PC=%#llx.%s\n", YELL, (unsigned long long)emu_.cpu.regs.pc, RST);
        print_diagnostics("");
        return 0;
    }
    if (!failed_) {
        stage("Attaching ARM64 execution backend", [&] {
            std::string err;
            if (!emu_.backend->attach(*emu_.ram, emu_.bus, err))
                throw std::runtime_error(err);
            emu_.backend->set_state(emu_.cpu);
            return std::string(emu_.backend->name());
        });
    }

    // 9) Execute the real Quest 2 kernel -------------------------------------
    if (!failed_) {
        std::printf("[%s....%s] Executing Quest 2 kernel\n", YELL, RST);
        std::fflush(stdout);
        cpu::RunResult rr = emu_.backend->run(emu_.config.max_instructions);
        if (rr.ok()) {
            std::printf("[ %sOK%s ] Kernel execution ran  %s%s (%llu insns, PC=%#llx)%s\n",
                        GREEN, RST, DIM, rr.detail.c_str(),
                        (unsigned long long)rr.instructions_executed,
                        (unsigned long long)rr.pc, RST);
            print_diagnostics("");
        } else if (rr.kind == cpu::RunResult::Kind::Spin) {
            // Not a crash: the CPU really executed, then got stuck busy-waiting on
            // hardware state that never changes -- this IS the next blocker.
            std::printf("[%sHANG%s] Kernel is spinning  %s%s%s\n",
                        YELL, RST, YELL, rr.detail.c_str(), RST);
            std::printf("       %sreal execution confirmed: %llu instructions ran before the "
                        "spin-wait at PC=%#llx.%s\n", DIM,
                        (unsigned long long)rr.instructions_executed,
                        (unsigned long long)rr.pc, RST);
            print_arm64_exception(rr);
            print_diagnostics("");
            failed_ = true;
        } else {
            std::printf("[%sFAIL%s] Kernel execution stopped  %s%s%s\n",
                        RED, RST, RED, rr.detail.c_str(), RST);
            failed_ = true;
            print_arm64_exception(rr);
            print_diagnostics("Executing Quest 2 kernel");
        }
    }

    return failed_ ? 1 : 0;
}

void BootPipeline::print_arm64_exception(const cpu::RunResult& rr) {
    if (!emu_.backend) return;
    cpu::Aarch64Regs r = emu_.backend->read_regs();

    std::printf("\n%s=== ARM64 EXCEPTION ===%s\n\n", BOLD, RST);
    std::printf("  PC     : %#018llx\n", (unsigned long long)r.pc);
    std::printf("  SP     : %#018llx\n", (unsigned long long)r.sp);
    std::printf("  PSTATE : %#018llx\n\n", (unsigned long long)r.pstate);
    for (int i = 0; i < 31; i += 2) {
        if (i + 1 < 31)
            std::printf("  X%-2d: %#018llx    X%-2d: %#018llx\n",
                        i, (unsigned long long)r.x[i], i + 1, (unsigned long long)r.x[i + 1]);
        else
            std::printf("  X%-2d: %#018llx\n", i, (unsigned long long)r.x[i]);
    }

    std::printf("\nFault:\n");
    if (rr.kind == cpu::RunResult::Kind::MemFault)
        std::printf("  Type    : unclaimed %s\n  Address : %#llx\n  Size    : %u bytes\n",
                    rr.fault_is_write ? "write" : "read",
                    (unsigned long long)rr.fault_addr, rr.fault_size);
    else
        std::printf("  Type    : %s\n", rr.detail.c_str());

    if (rr.last_mmio_valid)
        std::printf("\nLast MMIO:\n  %s %#llx (issued from PC %#llx)\n",
                    rr.last_mmio_write ? "WRITE" : "READ",
                    (unsigned long long)rr.last_mmio_addr, (unsigned long long)rr.last_mmio_pc);

    std::printf("\nExecution:\n  Instructions executed : %llu\n",
                (unsigned long long)rr.instructions_executed);

    // Dump the instruction words around the faulting PC.
    uint8_t buf[32] = {};
    uint64_t start = (r.pc >= 8) ? r.pc - 8 : 0;
    if (emu_.backend->read_mem(start, buf, sizeof(buf))) {
        std::printf("\nCode around PC:\n");
        for (int i = 0; i < 32; i += 4) {
            uint32_t w = buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
            uint64_t a = start + i;
            std::string dis = emu_.backend->disasm_at(a);
            std::printf("  %#010llx: %08x  %-24s%s\n", (unsigned long long)a, w,
                        dis.c_str(), a == r.pc ? " <== PC" : "");
        }
    }
    // The last instructions executed before the stop (how we got here).
    auto pcs = emu_.backend->recent_pcs();
    if (!pcs.empty()) {
        std::printf("\nRecent instruction trace (last %zu, oldest first):\n", pcs.size());
        size_t start = pcs.size() > 20 ? pcs.size() - 20 : 0;
        for (size_t i = start; i < pcs.size(); ++i)
            std::printf("  %#018llx: %s\n", (unsigned long long)pcs[i],
                        emu_.backend->disasm_at(pcs[i]).c_str());
    }

    // If the MMU was on when we stopped, walk the guest page tables to prove
    // whether the faulting/spinning VA is actually mapped.
    walk_page_table(r.pc);
    // Recover whatever the kernel logged (the panic/failure reason lives here
    // even when no console device emitted it).
    dump_kmsg();
    std::printf("\n");
}

// Software AArch64 stage-1 translation-table walk, assuming the standard config
// this kernel uses (VA_BITS=39, 4KB granule, 3 levels; TCR confirmed T0SZ=25).
// Page tables are read from guest RAM by *physical* address (bypassing the MMU),
// which is exactly what the hardware walker does.
void BootPipeline::walk_page_table(uint64_t va) {
    if (!emu_.backend || !emu_.ram) return;
    cpu::MmuRegs m = emu_.backend->read_mmu_regs();
    if (!m.valid) return;

    const bool high = (va >> 39) != 0;          // kernel-half VAs use TTBR1
    uint64_t table = (high ? m.ttbr1 : m.ttbr0) & 0x0000fffffffff000ull;

    std::printf("\nPage-table walk of %#llx  (%s=%#llx, VA39/4KB):\n",
                (unsigned long long)va, high ? "TTBR1" : "TTBR0",
                (unsigned long long)table);
    std::printf("  ESR_EL1=%#llx  FAR_EL1=%#llx  VBAR_EL1=%#llx  MAIR_EL1=%#llx\n",
                (unsigned long long)m.esr, (unsigned long long)m.far_el1,
                (unsigned long long)m.vbar, (unsigned long long)m.mair);
    if (m.esr) {
        unsigned ec = (unsigned)((m.esr >> 26) & 0x3f);
        const char* what = "?";
        switch (ec) {
            case 0x00: what = "unknown/uncategorized"; break;
            case 0x0e: what = "illegal execution state"; break;
            case 0x15: what = "SVC (AArch64)"; break;
            case 0x18: what = "trapped MSR/MRS/system insn"; break;
            case 0x20: what = "instruction abort (lower EL)"; break;
            case 0x21: what = "instruction abort (same EL)"; break;
            case 0x22: what = "PC alignment fault"; break;
            case 0x24: what = "data abort (lower EL)"; break;
            case 0x25: what = "data abort (same EL)"; break;
            case 0x26: what = "SP alignment fault"; break;
        }
        std::printf("  ESR.EC=%#04x (%s)  ISS=%#llx\n", ec, what,
                    (unsigned long long)(m.esr & 0x1ffffff));
    }

    const int shift[3] = { 30, 21, 12 };        // L1, L2, L3 index shifts (4KB)
    const char* lvl[3] = { "L1", "L2", "L3" };
    uint64_t out = 0; bool mapped = false;
    try {
        for (int i = 0; i < 3; ++i) {
            uint64_t idx = (va >> shift[i]) & 0x1ff;
            uint64_t desc_addr = table + idx * 8;
            if (!emu_.ram->contains(desc_addr, 8)) {
                std::printf("  %s[%#llx] @ %#llx  <descriptor outside RAM>\n",
                            lvl[i], (unsigned long long)idx, (unsigned long long)desc_addr);
                break;
            }
            uint64_t desc = emu_.ram->read64(desc_addr);
            uint64_t next = desc & 0x0000fffffffff000ull;
            const char* kind = (desc & 1) == 0 ? "INVALID"
                             : ((desc & 3) == 3 ? (i == 2 ? "page" : "table")
                                                : "block");
            std::printf("  %s[%#05llx] @ %#llx = %#018llx  (%s)\n",
                        lvl[i], (unsigned long long)idx, (unsigned long long)desc_addr,
                        (unsigned long long)desc, kind);
            if ((desc & 1) == 0) { break; }                       // invalid -> stop
            if ((desc & 3) == 1) {                                // block
                uint64_t mask = (i == 0) ? 0x3fffffffull : 0x1fffffull;
                out = (next & ~mask) | (va & mask); mapped = true; break;
            }
            if (i == 2) { out = next | (va & 0xfff); mapped = true; break; }
            table = next;                                         // descend
        }
    } catch (const std::exception& e) {
        std::printf("  walk aborted: %s\n", e.what());
    }
    if (mapped) std::printf("  => PA %#llx  (MAPPED)\n", (unsigned long long)out);
    else        std::printf("  => UNMAPPED (translation fault)\n");
}

// Recover the Linux printk ring buffer directly from guest RAM. In 4.19 each
// record is a struct printk_log { u64 ts; u16 len; u16 text_len; u16 dict_len;
// u8 facility; u8 flags; } (16 bytes) followed by `text_len` bytes of message.
// We locate the first record by its "Linux version " text and walk forward.
// Qualcomm DTBs ship a /memory node with size 0; the bootloader (ABL) writes in
// the real RAM size before entering the kernel. We do the same, in place, so the
// kernel's memblock has usable memory (otherwise every early allocation fails).
void BootPipeline::patch_dtb_memory(Bytes& dtb) {
    if (!emu_.fdt || !emu_.fdt->root()) return;
    const boot::FdtNode* mem = nullptr;
    for (const auto& c : emu_.fdt->root()->children) {
        const boot::FdtProp* dt = c->prop("device_type");
        if (dt && dt->str() == "memory") { mem = c.get(); break; }
    }
    if (!mem) return;
    const boot::FdtProp* reg = mem->prop("reg");
    if (!reg) return;
    uint32_t ac = mem->address_cells(), sc = mem->size_cells();
    if (reg->data.size() < (size_t)(ac + sc) * 4) return;

    // Log every existing (base,size) entry so we can see the raw layout.
    {
        auto regs = mem->reg();
        HW_INFO("boot.dtb", "/memory '{}' ac={} sc={} {} entries", mem->name, ac, sc, regs.size());
        for (auto& e : regs) HW_INFO("boot.dtb", "  entry base={:#x} size={:#x}", e.first, e.second);
    }

    auto be32 = [](const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; };
    uint64_t old = 0;
    for (uint32_t i = 0; i < sc; ++i) old = (old << 32) | be32(reg->data.data() + ac * 4 + i * 4);
    emu_.dtb_mem_size_old = old;

    // The Qualcomm DTB ships /memory = <0 0> (base AND size); ABL writes both.
    // Emulate that: base = our DRAM base, size = our guest RAM size.
    auto write_cells = [&](size_t off, uint32_t cells, uint64_t value) {
        if (off + (size_t)cells * 4 > dtb.size()) return;
        for (uint32_t i = 0; i < cells; ++i) {
            uint32_t cell = (uint32_t)((value >> ((cells - 1 - i) * 32)) & 0xffffffffull);
            dtb[off + i * 4 + 0] = (cell >> 24) & 0xff;
            dtb[off + i * 4 + 1] = (cell >> 16) & 0xff;
            dtb[off + i * 4 + 2] = (cell >> 8) & 0xff;
            dtb[off + i * 4 + 3] = cell & 0xff;
        }
    };
    write_cells(reg->blob_offset, ac, emu_.dram_base);                 // base
    write_cells(reg->blob_offset + (size_t)ac * 4, sc, emu_.ram_size); // size
    HW_INFO("boot.dtb", "/memory patched: base->{:#x} size {:#x}->{:#x} (ac={} sc={})",
            emu_.dram_base, old, emu_.ram_size, ac, sc);

    // Corruption check: the patch must touch ONLY the reg cells we wrote.
    if (dtb.size() == emu_.boot_img.dtb.size()) {
        size_t ndiff = 0, first = 0, last = 0;
        for (size_t i = 0; i < dtb.size(); ++i)
            if (dtb[i] != emu_.boot_img.dtb[i]) { if (!ndiff) first = i; last = i; ++ndiff; }
        size_t wr_lo = reg->blob_offset, wr_hi = reg->blob_offset + (size_t)(ac + sc) * 4;
        HW_WARN("boot.dtb", "patch diff: {} bytes in [{:#x}..{:#x}]; write-window [{:#x}..{:#x}]{}",
                ndiff, first, last, wr_lo, wr_hi,
                (ndiff && (first < wr_lo || last >= wr_hi)) ? "  <-- OUT OF WINDOW: CORRUPTION" : "");
    } else {
        HW_WARN("boot.dtb", "patch changed blob SIZE {}->{}: CORRUPTION",
                emu_.boot_img.dtb.size(), dtb.size());
    }
}

void BootPipeline::dump_dt() {
    if (!emu_.fdt) { std::printf("(no DTB)\n"); return; }
    auto dump_node = [&](const boot::FdtNode* n, const char* label) {
        if (!n) { std::printf("%s: <not found>\n", label); return; }
        std::printf("\n%s%s '%s'%s\n", BOLD, label, n->name.c_str(), RST);
        for (const auto& p : n->props) {
            std::printf("  %-20s ", p.name.c_str());
            // Heuristic: print short props as cell lists, longer as string.
            if (p.name == "compatible" || p.name == "clock-names" || p.name == "status" ||
                p.name == "clock-output-names") {
                for (auto& s : p.strlist()) std::printf("\"%s\" ", s.c_str());
            } else if (p.data.size() % 4 == 0 && p.data.size() <= 64) {
                for (size_t i = 0; i < p.data.size() / 4; ++i) std::printf("%#x ", p.u32(i));
            } else {
                std::printf("(%zu bytes)", p.data.size());
            }
            std::printf("\n");
        }
    };
    const boot::FdtNode* t = emu_.fdt->find_compatible("arm,armv8-timer");
    if (!t) t = emu_.fdt->find_compatible("arm,armv7-timer");
    dump_node(t, "arch timer");
    // Walk the timer's ancestry to see how interrupt-parent resolves.
    std::printf("\n%stimer ancestry (interrupt-parent chain)%s\n", BOLD, RST);
    for (const boot::FdtNode* n = t; n; n = n->parent) {
        const boot::FdtProp* ip = n->prop("interrupt-parent");
        const boot::FdtProp* ic = n->prop("#interrupt-cells");
        std::printf("  '%s' interrupt-parent=%s #interrupt-cells=%s\n",
                    n->name.empty() ? "/" : n->name.c_str(),
                    ip ? std::to_string(ip->u32()).c_str() : "-",
                    ic ? std::to_string(ic->u32()).c_str() : "-");
    }
    const boot::FdtNode* g = emu_.fdt->find_compatible("arm,gic-v3");
    dump_node(g, "GICv3");
    if (g) {
        const boot::FdtProp* ph = g->prop("phandle");
        const boot::FdtProp* lph = g->prop("linux,phandle");
        std::printf("  GIC phandle=%s linux,phandle=%s\n",
                    ph ? std::to_string(ph->u32()).c_str() : "-",
                    lph ? std::to_string(lph->u32()).c_str() : "-");
    }
    // memory-mapped timer(s), if any
    dump_node(emu_.fdt->find_compatible("arm,armv7-timer-mem"), "mem-mapped timer");
    dump_node(emu_.fdt->find_compatible("arm,mmio-timer"), "qcom mmio timer");

    // CRITICAL CHECK: the guest kernel unflattens the PATCHED blob (dtb_image),
    // while everything above inspects the UNPATCHED emu_.fdt. Byte-diff the two:
    // the ONLY differences should be inside the /memory reg cells. Any other
    // differing offset means patch_dtb_memory corrupted the structure.
    if (!emu_.dtb_image.empty() && emu_.dtb_image.size() == emu_.boot_img.dtb.size()) {
        std::printf("\n%s== patched vs unpatched DTB byte-diff ==%s\n", BOLD, RST);
        size_t ndiff = 0, first = 0, last = 0;
        for (size_t i = 0; i < emu_.dtb_image.size(); ++i) {
            if (emu_.dtb_image[i] != emu_.boot_img.dtb[i]) {
                if (ndiff == 0) first = i;
                last = i; ++ndiff;
            }
        }
        std::printf("  %zu bytes differ, range [%#zx .. %#zx] (contiguous run expected inside /memory reg)\n",
                    ndiff, first, last);
    } else if (!emu_.dtb_image.empty()) {
        std::printf("\n  WARNING: patched blob size (%zu) != unpatched (%zu) -- structure changed!\n",
                    emu_.dtb_image.size(), emu_.boot_img.dtb.size());
    }
    std::printf("\n");
}

void BootPipeline::dump_kmsg() {
    if (!emu_.ram) return;
    auto sp = emu_.ram->span();
    size_t scan = sp.size();   // full RAM: the relocated log buffer can be high
    const char* needle = "Linux version ";
    const size_t nlen = 14;

    auto rd16 = [&](size_t o) -> uint16_t { return (uint16_t)(sp[o] | (sp[o + 1] << 8)); };

    // "Linux version" appears as the .rodata banner AND as the first record of
    // the printk ring buffer -- and the kernel relocates that buffer during boot
    // (setup_log_buf), leaving several copies. Find every candidate whose 16-byte
    // prefix is a valid printk_log header, then pick the one whose record chain
    // is longest (the active/relocated buffer, which holds the newest messages
    // including any panic).
    auto count_chain = [&](size_t start) -> size_t {
        size_t rec = start, n = 0;
        while (rec + 16 <= sp.size() && n < 40000) {
            uint16_t len = rd16(rec + 8), tl = rd16(rec + 10);
            if (len < 16 || len > 8192 || rec + 16 + tl > sp.size()) break;
            ++n; rec += len;
        }
        return n;
    };
    size_t rec = SIZE_MAX, best = 0;
    for (size_t i = 16; i + nlen < scan; ++i) {
        if (sp[i] != 'L' || std::memcmp(sp.data() + i, needle, nlen) != 0) continue;
        size_t h = i - 16;
        uint16_t len = rd16(h + 8), text_len = rd16(h + 10);
        if (text_len >= nlen && text_len <= 800 && len >= (uint16_t)(16 + text_len) && len <= 4096) {
            size_t c = count_chain(h);
            if (c > best) { best = c; rec = h; }
        }
    }
    if (rec == SIZE_MAX) { std::printf("\n(kernel printk ring buffer not found in RAM)\n"); return; }

    std::printf("\n%s== recovered kernel log (printk @ phys %#llx) ==%s\n",
                BOLD, (unsigned long long)(emu_.ram->base() + rec), RST);
    int printed = 0;
    for (int n = 0; n < 40000 && rec + 16 <= sp.size(); ++n) {
        uint16_t len = rd16(rec + 8);
        uint16_t text_len = rd16(rec + 10);
        if (len < 16 || len > 8192 || rec + 16 + text_len > sp.size()) break;  // corrupt / end
        if (text_len > 0 && text_len <= 1024) {
            std::string line(reinterpret_cast<const char*>(sp.data() + rec + 16), text_len);
            bool printable = true;
            for (char c : line)
                if ((unsigned char)c < 0x09 || ((unsigned char)c > 0x0d && (unsigned char)c < 0x20)) { printable = false; break; }
            if (printable) { std::printf("  %s\n", line.c_str()); printed++; }
        }
        rec += len;   // advance regardless, so one odd record doesn't hide the rest
    }
    if (!printed) std::printf("  (no readable records)\n");

    // Raw fallback: the panic/oops text may live in a relocated buffer we didn't
    // pick. Scan all of RAM for key phrases and print the readable line at each.
    const char* keys[] = { "not syncing:", "Unable to handle", "Internal error",
                           "kernel BUG at", "Kernel panic" };
    std::printf("%s-- panic/BUG text found in RAM --%s\n", DIM, RST);
    int hits = 0;
    for (const char* key : keys) {
        size_t klen = std::strlen(key);
        for (size_t i = 0; i + klen < scan && hits < 40; ++i) {
            if (sp[i] != (uint8_t)key[0] || std::memcmp(sp.data() + i, key, klen) != 0) continue;
            // back up to the start of the printable line, then print it
            size_t s = i; while (s > 0 && sp[s - 1] >= 0x20 && sp[s - 1] < 0x7f && i - s < 160) --s;
            size_t e = i; while (e < sp.size() && sp[e] >= 0x20 && sp[e] < 0x7f && e - s < 300) ++e;
            std::string line(reinterpret_cast<const char*>(sp.data() + s), e - s);
            i = e;  // skip past this line
            if (line.find('%') != std::string::npos) continue;  // skip .rodata format templates
            std::printf("  %s\n", line.c_str());
            hits++;
        }
    }
    if (!hits) std::printf("  (none)\n");
}

void BootPipeline::print_diagnostics(const std::string& failed_stage) {
    std::printf("\n%s== diagnostics ==%s\n", BOLD, RST);

    std::printf("%sCPU%s\n", BOLD, RST);
    std::printf("  Architecture : ARM64 (AArch64)\n");
    std::printf("  Backend      : %s\n", emu_.backend ? emu_.backend->name() : "none (not yet attached)");
    std::printf("  Entry PC     : %#llx\n", (unsigned long long)emu_.cpu.regs.pc);
    std::printf("  x0 (DTB ptr) : %#llx\n", (unsigned long long)emu_.cpu.regs.x[0]);

    std::printf("%sMemory%s\n", BOLD, RST);
    if (emu_.ram) {
        std::printf("  Guest RAM    : %llu MB @ %#llx-%#llx\n",
                    (unsigned long long)(emu_.ram->size() / (1024 * 1024)),
                    (unsigned long long)emu_.ram->base(), (unsigned long long)emu_.ram->end());
    } else {
        std::printf("  Guest RAM    : (not allocated)\n");
    }

    std::printf("%sKernel%s\n", BOLD, RST);
    std::printf("  Android build: %s\n", emu_.android_build.empty() ? "(unknown)" : emu_.android_build.c_str());
    std::printf("  Image        : %s (%zu KB)\n",
                emu_.boot_img.kernel.empty() ? "not loaded" : "loaded",
                emu_.boot_img.kernel.size() / 1024);
    std::printf("  Entry        : %#llx\n", (unsigned long long)emu_.kernel_load);
    std::printf("  DTB          : %s @ %#llx\n",
                emu_.boot_img.dtb.empty() ? "none" : "loaded", (unsigned long long)emu_.dtb_load);
    std::printf("  Initramfs    : %s @ %#llx (%llu KB)\n",
                emu_.ramdisk_load ? "loaded" : "none", (unsigned long long)emu_.ramdisk_load,
                (unsigned long long)(emu_.ramdisk_size / 1024));
    std::printf("  Cmdline      : %s\n", emu_.boot_img.cmdline.empty() ? "(none)" : emu_.boot_img.cmdline.c_str());

    std::printf("%sDevices%s\n", BOLD, RST);
    for (const auto& d : emu_.bus.devices()) {
        const char* st = dev::status_str(d->status());
        const char* col = d->status() == dev::DevStatus::Ok ? GREEN
                        : d->status() == dev::DevStatus::Fail ? RED : YELL;
        std::printf("  %-16s @ %#010llx  %s%s%s\n", d->name(),
                    (unsigned long long)d->base(), col, st, RST);
    }
    // Subsystems that are known-required but not yet modeled (from RE findings).
    for (const char* planned : { "arch_timer", "ufs_storage", "syncboss_spi",
                                 "kgsl_gpu", "display_dsi", "sensors" }) {
        std::printf("  %-16s %s          %sPLANNED%s\n", planned, "          ", DIM, RST);
    }

    if (!emu_.bus.unclaimed().empty()) {
        std::printf("%sUnclaimed MMIO (candidates for next device)%s\n", BOLD, RST);
        size_t shown = 0;
        for (const auto& u : emu_.bus.unclaimed()) {
            if (shown++ >= 12) { std::printf("  ... and %zu more\n", emu_.bus.unclaimed().size() - 12); break; }
            std::printf("  %s %#llx  x%llu\n", u.is_write ? "W" : "R",
                        (unsigned long long)u.addr, (unsigned long long)u.count);
        }
    }

    if (!failed_stage.empty()) {
        std::printf("\n%sBoot stalled at stage:%s %s\n", RED, RST, failed_stage.c_str());
        std::printf("Last successful stage : %s\n", last_ok_.empty() ? "(none)" : last_ok_.c_str());
    }
    std::printf("\n");
}

} // namespace hw::core
