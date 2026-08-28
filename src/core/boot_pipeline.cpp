#include "core/boot_pipeline.h"
#include "common/compress.h"
#include "common/log.h"
#include "devices/geni_uart.h"
#include "devices/gic.h"
#include "devices/qcom_rsc.h"
#include "devices/qcom_gcc.h"
#include "devices/qcom_ufs.h"
#include "devices/qcom_rng.h"
#include "devices/qcom_sec_engine.h"
#include "devices/qcom_tcsr.h"
#include "devices/ufs_disk.h"
#include "devices/super.h"
#include "devices/stub_device.h"
#include "platform/qcom_cmddb.h"
#include "ota/payload.h"
#include "ota/zip_reader.h"
#include "platform/kona.h"

#include <algorithm>
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

        // RPMh RSC (apps_rsc @ 0x18200000, drv-0/1/2). With the STANDALONE cmd-db
        // below, the rpmh vote paths are no-ops, so this only needs to let the
        // rpmh-rsc driver probe (config register) and satisfy write-sync readbacks
        // -- which in turn lets clk-rpmh + the rpmh-regulators + GCC come up so the
        // UFS clocks/regulators resolve. (kona.dtsi: reg 0x18200000 x3 @0x10000.)
        emu_.bus.add(std::make_unique<dev::QcomRsc>(0x18200000ull, 0x30000ull, "apps_rsc"));

        // Global Clock Controller (gcc-kona @ 0x100000, 0x1f0000). Models the RCG
        // update / branch CLK_OFF / GDSC PWR_ON / GPLL0-lock status bits the UFS
        // clock+power bring-up polls on (permissive zero-RAM leaves them stuck).
        emu_.bus.add(std::make_unique<dev::QcomGcc>(0x100000ull, 0x1f0000ull));

        // PRNG (msm_rng @ 0x793000). Supplies hardware entropy so the hwrng
        // kthread stops hot-looping ("no data available") and crng init proceeds.
        emu_.bus.add(std::make_unique<dev::QcomRng>(0x793000ull, 0x1000ull));

        // Phase 11 (tz only): the secure crypto/PRNG engine at 0x791000. tz's cold
        // boot initializes it (write enable bits + poll readback, read a ready bit,
        // read entropy). Modeled as a register block with readback + forced status.
        // Added only in tz mode so the kernel boot is unchanged.
        if (emu_.config.tz_boot) {
            emu_.bus.add(std::make_unique<dev::QcomSecEngine>(0x791000ull, 0x1000ull));
            // TCSR SoC-info block QSEE parses during cold boot (0x1fc8000).
            emu_.bus.add(std::make_unique<dev::QcomTcsr>(0x1fc8000ull, 0x1000ull));
        }

        return std::format("dram_base={:#x}, {} MMIO devices", emu_.dram_base, emu_.bus.devices().size());
    });

    if (emu_.config.list_dt && !failed_) { list_dt(); return 0; }
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

        // Populate the Qualcomm Command DB (normally written by XBL/SBL) into its
        // reserved region. Without it the whole rpmh clock/regulator tree defers
        // ("Invalid Command DB Magic") and UFS never probes. STANDALONE bit set ->
        // rpmh votes are no-ops (no RSC command traffic needed).
        {
            Bytes cdb = platform::build_cmd_db();
            if (emu_.ram->contains(platform::kCmdDbBase, cdb.size())) {
                emu_.ram->load(platform::kCmdDbBase, cdb);
                HW_INFO("boot.mem", "cmd-db: wrote {}-byte STANDALONE blob @ {:#x}",
                        cdb.size(), platform::kCmdDbBase);
            }
        }

        // UFS host controller (ufshc @ 0x1d84000, IRQ = GIC SPI 265 -> INTID 297).
        // Created here (not in the device stage) because it DMAs guest RAM, which
        // exists only now. Backed by a synthesized GPT disk served from the OTA.
        {
            const std::string suffix = emu_.config.slot_suffix.empty() ? "_a" : emu_.config.slot_suffix;
            // Reconstruct any OTA payload partition (by base name) into its full
            // image, lazily + cached. Shared by the static-partition source and the
            // super/liblp extent server.
            auto ota_extract = [this, zip, &payload](const std::string& base) -> Bytes {
                const ota::Partition* pp = zip ? payload.find(base) : nullptr;
                if (!pp) return {};
                // Persistent decompression cache: XZ-inflating ~2.8GB of logical
                // partitions every run is the wall-clock bottleneck, so cache the
                // reconstructed images and reuse them when the size matches.
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path cache = fs::path("build") / "ota_cache" / (base + ".img");
                if (fs::exists(cache, ec) && fs::file_size(cache, ec) == pp->size) {
                    Bytes b(pp->size);
                    std::ifstream in(cache, std::ios::binary);
                    if (in.read(reinterpret_cast<char*>(b.data()), (std::streamsize)b.size())) {
                        HW_WARN("boot.ufs", "[LP] loaded '{}' from cache ({} MB)", base, b.size()/(1024*1024));
                        return b;
                    }
                }
                Bytes img = ota::extract_partition(*zip, payload, base);
                fs::create_directories(cache.parent_path(), ec);
                std::ofstream out(cache, std::ios::binary);
                out.write(reinterpret_cast<const char*>(img.data()), (std::streamsize)img.size());
                return img;
            };

            // Build the physical `super` (dynamic partitions) from the OTA's
            // dynamic_partition_metadata: the logical partitions (system/vendor/...)
            // become LINEAR extents inside super with synthesized liblp metadata.
            std::shared_ptr<dev::SuperImage> super;
            if (payload.dap.present && !payload.dap.groups.empty()) {
                const auto& g = payload.dap.groups[0];
                std::vector<dev::SuperImage::Logical> logicals;
                for (const auto& base : g.partition_names) {
                    const ota::Partition* pp = payload.find(base);
                    if (pp) logicals.push_back({ base + suffix, base, pp->size });
                }
                super = std::make_shared<dev::SuperImage>(
                    std::move(logicals), g.name + suffix, g.maximum_size, ota_extract);
            }

            auto disk = std::make_shared<dev::UfsDisk>(super);
            // Static A/B partitions (vbmeta*/boot/dtbo): active-slot "<part>_a" is
            // served from OTA "<part>"; inactive _b + scratch (metadata/misc/
            // userdata) stay zero.
            disk->set_partition_source([suffix, ota_extract](const std::string& gpt_name) -> Bytes {
                if (gpt_name.size() <= suffix.size() ||
                    gpt_name.compare(gpt_name.size() - suffix.size(), suffix.size(), suffix) != 0)
                    return {};
                return ota_extract(gpt_name.substr(0, gpt_name.size() - suffix.size()));
            });
            emu_.bus.add(std::make_unique<dev::QcomUfs>(0x1d84000ull, 0x3000ull, 297,
                                                        emu_.ram.get(), disk));
            emu_.bus.add(std::make_unique<dev::QcomIce>(0x1d90000ull, 0x8000ull));    // inline crypto
            emu_.bus.add(std::make_unique<dev::QcomUfsPhy>(0x1d87000ull, 0x1000ull)); // UFS QMP-v4 PHY
            HW_INFO("boot.mem", "ufs: controller @ 0x1d84000 (irq 297), disk {} blocks x {}B, {} parts",
                    disk->block_count(), disk->block_size(), disk->parts().size());
        }

        // Kernel load address. Per the ARM64 boot protocol the Image must sit at
        // `text_offset` bytes above a 2MB-aligned base. The boot image's
        // kernel_addr (offset 0x8000) is NOT what the kernel assumes -- using it
        // makes map_kernel() BUG on the alignment. Use the Image header's
        // text_offset from a 2MB-aligned DRAM base.
        // The stock Quest DTB reserves the low ~200MB (from 0x80000000) for the
        // hypervisor, XBL, SMEM, cmd-db and a large no-map removed_region. Loading
        // the kernel at the RAM base (0x80080000) puts the image, DTB and initramfs
        // INSIDE no-map holes -- the guest has no struct pages there, so its buddy
        // allocator hands the same physical page out twice (userspace page aliasing
        // + "Bad page state"). A real bootloader loads Linux in genuine usable RAM
        // above those firmware regions; do the same: find the first free 2MB-aligned
        // run large enough for the kernel image + DTB + initramfs together.
        uint64_t kspan = round_up(std::max<uint64_t>(emu_.boot_img.kernel.size(),
                                                     emu_.boot_img.kernel_image_size), 0x200000);
        uint64_t rd_span  = round_up(emu_.boot_img.ramdisk.size() + 0x100000, 0x100000);
        uint64_t dtb_span = round_up(emu_.boot_img.dtb.size() + 0x10000, 0x100000);
        uint64_t need = kspan + dtb_span + rd_span + 0x400000;     // + slack
        uint64_t base2m = find_free_load_base(need, 0x200000);
        if (!base2m) {                                            // no gap: fall back to base
            base2m = round_up(emu_.dram_base, 0x200000);
            HW_WARN("boot.mem", "no reserved-safe region for {:#x} bytes; using base {:#x}",
                    need, base2m);
        } else {
            HW_INFO("boot.mem", "load base {:#x} (need {:#x}) -- avoids all reserved-memory",
                    base2m, need);
        }
        emu_.kernel_load = base2m + emu_.boot_img.kernel_text_offset;
        if (!emu_.ram->contains(emu_.kernel_load, emu_.boot_img.kernel.size()))
            emu_.kernel_load = emu_.dram_base + 0x80000;
        emu_.ram->load(emu_.kernel_load, emu_.boot_img.kernel);

        // Place DTB and ramdisk sequentially after the kernel image (all inside the
        // reserved-safe run chosen above, so none overlap a no-map region).
        uint64_t cursor = emu_.kernel_load + kspan;

        if (!emu_.boot_img.dtb.empty()) {
            // Emulate the bootloader: patch the /memory node with the real RAM size.
            emu_.dtb_image.assign(emu_.boot_img.dtb.begin(), emu_.boot_img.dtb.end());
            apply_dtb_profile(emu_.dtb_image);   // minimal profile: disable chosen nodes
            patch_dtb_memory(emu_.dtb_image);
            emu_.dtb_load = cursor;
            // Advertise the initramfs so the kernel unpacks it instead of trying to
            // mount a (nonexistent) block root. The ramdisk lands after the DTB; we
            // reserve 4KB slack for the two /chosen properties we add, then write
            // /chosen/linux,initrd-start / -end (8-byte big-endian phys addresses).
            if (!emu_.boot_img.ramdisk.empty()) {
                emu_.ramdisk_load = round_up(emu_.dtb_load + emu_.dtb_image.size() + 0x1000, 0x100000);
                uint64_t rd_end = emu_.ramdisk_load + emu_.boot_img.ramdisk.size();
                auto be64 = [](uint64_t v){ Bytes b(8); for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (56 - 8*i)); return b; };
                for (auto [name, val] : { std::pair{std::string("linux,initrd-start"), emu_.ramdisk_load},
                                          std::pair{std::string("linux,initrd-end"),   rd_end} }) {
                    bool mod = false; std::string p; Bytes v = be64(val);
                    Bytes nb = boot::fdt_add_prop(emu_.dtb_image, "/chosen", name, v, mod, p);
                    if (mod) { emu_.dtb_image = std::move(nb);
                        HW_INFO("boot.dtb", "/chosen {} = {:#x}", name, val);
                    } else HW_WARN("boot.dtb", "could not add /chosen/{} (no /chosen node?)", name);
                }
            }
            // Set the full kernel command line the bootloader (ABL) supplies. The
            // stock DTB /chosen/bootargs is only a tuning stub (rcu/kpti); ABL
            // appends the boot-image cmdline plus androidboot.* runtime params. In
            // particular Android first-stage init needs androidboot.slot_suffix to
            // resolve `slotselect` fstab entries (without it: "Error updating for
            // slotselect" -> FirstStageMount fails) and androidboot.boot_devices to
            // build the by-name/ symlink path for the UFS controller.
            {
                std::string full;
                if (const boot::FdtNode* ch = emu_.fdt->find("/chosen"))
                    if (const boot::FdtProp* ba = ch->prop("bootargs")) full = ba->str();
                if (!emu_.boot_img.cmdline.empty()) { if (!full.empty()) full += " "; full += emu_.boot_img.cmdline; }
                full += " androidboot.slot_suffix=" + emu_.config.slot_suffix;
                full += " androidboot.boot_devices=soc/1d84000.ufshc";
                // androidboot.bootdevice (SINGULAR) -> ro.boot.bootdevice, which the
                // target init.kona.rc substitutes into
                //   symlink /dev/block/platform/soc/${ro.boot.bootdevice} /dev/block/bootdevice
                // Without it that symlink is never created, so /dev/block/bootdevice/by-name/*
                // is absent and vendor.qseecomd's open of by-name/ssd (O_SYNC) returns ENOENT
                // -> "SSD_INIT failed, shall not start listener services" -> qseecomd exit 255,
                // which gates the whole QSEE listener/keymaster chain. boot_devices (plural,
                // above) drives first-stage's /dev/block/by-name; bootdevice (singular) drives
                // the /dev/block/bootdevice tree the secure daemons use. The real ABL sets both.
                full += " androidboot.bootdevice=1d84000.ufshc";
                // Large kernel log ring so the full boot's init/service messages
                // survive (default ring wraps and evicts early service starts/exits).
                full += " log_buf_len=16M";
                // Verified Boot: present an unlocked bootloader (as a fastboot-
                // unlocked Quest does) so first-stage AVB is non-fatal on the vbmeta
                // digest, with a valid hash algorithm so the digest parse succeeds.
                // The vbmeta/partition data is the real signed OTA content; dm-verity
                // still uses the real hashtree descriptors from it.
                full += " androidboot.verifiedbootstate=orange";
                full += " androidboot.vbmeta.device_state=unlocked";
                full += " androidboot.vbmeta.hash_alg=sha256";
                Bytes v(full.begin(), full.end()); v.push_back(0);   // NUL-terminated string
                bool mod = false; std::string p;
                Bytes nb = boot::fdt_set_prop(emu_.dtb_image, "/chosen", "bootargs", v, mod, p);
                if (mod) { emu_.dtb_image = std::move(nb);
                    HW_INFO("boot.dtb", "/chosen/bootargs = {}", full);
                } else HW_WARN("boot.dtb", "could not set /chosen/bootargs");
            }
            // Enable the UFS storage stack. The stock boot.img DTB ships the UFS
            // controller + PHY DISABLED (with the PHY missing its compatible); on
            // real hardware the ABL-applied dtbo overlay (hollywood-ufs.dtsi) sets
            // status="ok", the PHY compatible, and the PMIC supplies. We replicate
            // just the UFS portion so the real ufshcd/ufs-qcom stack probes without
            // re-enabling the display/camera we disabled for the minimal profile.
            {
                auto set_bytes = [&](const char* path, const char* name, Bytes v) {
                    bool m = false; std::string p;
                    Bytes nb = boot::fdt_set_prop(emu_.dtb_image, path, name, v, m, p);
                    if (m) emu_.dtb_image = std::move(nb);
                    else HW_WARN("boot.dtb", "UFS: could not set {} {}", path, name);
                };
                auto set_str  = [&](const char* path, const char* name, const std::string& val) {
                    Bytes v(val.begin(), val.end()); v.push_back(0); set_bytes(path, name, std::move(v)); };
                auto set_cell = [&](const char* path, const char* name, uint32_t val) {
                    Bytes v{ (uint8_t)(val>>24),(uint8_t)(val>>16),(uint8_t)(val>>8),(uint8_t)val };
                    set_bytes(path, name, std::move(v)); };
                set_str("/soc/ufshc@1d84000",      "status",     "ok");
                set_str("/soc/ufsphy_mem@1d87000", "status",     "ok");
                set_str("/soc/ufsphy_mem@1d87000", "compatible", "qcom,ufs-phy-qmp-v4");
                // PHY PMIC supplies (hollywood-ufs.dtsi): vdda-phy=L5A, vdda-pll=L9A.
                // The qmp-v4 PHY driver requires these (unlike the controller, which
                // tolerates dummy regulators) or it NULL-derefs in cfg_vreg.
                set_cell("/soc/ufsphy_mem@1d87000", "vdda-phy-supply", 104);       // &L5A
                set_cell("/soc/ufsphy_mem@1d87000", "vdda-pll-supply", 105);       // &L9A
                set_cell("/soc/ufsphy_mem@1d87000", "vdda-phy-max-microamp", 89900);
                set_cell("/soc/ufsphy_mem@1d87000", "vdda-pll-max-microamp", 18800);
                set_bytes("/soc/ufsphy_mem@1d87000","vdda-phy-always-on", Bytes{});  // bool
                // Controller PMIC supplies (VCC=L17A, VCCQ=L6A, VCCQ2=S4A, vdd-hba=gdsc).
                set_cell("/soc/ufshc@1d84000", "vcc-supply",    570);   // &L17A
                set_cell("/soc/ufshc@1d84000", "vccq-supply",   564);   // &L6A
                set_cell("/soc/ufshc@1d84000", "vccq2-supply",  143);   // &S4A
                set_cell("/soc/ufshc@1d84000", "vdd-hba-supply", 98);   // &ufs_phy_gdsc
                // Regulator limits ufshcd_populate_vreg requires once a supply is
                // present (hollywood-ufs.dtsi values).
                set_bytes("/soc/ufshc@1d84000", "vdd-hba-fixed-regulator", Bytes{});   // bool
                set_cell("/soc/ufshc@1d84000", "vcc-max-microamp",   800000);
                set_cell("/soc/ufshc@1d84000", "vccq-max-microamp",  800000);
                set_cell("/soc/ufshc@1d84000", "vccq2-max-microamp", 800000);
                { Bytes vl{0,0x26,0x35,0xc0, 0,0x2d,0x02,0xe0}; // vcc-voltage-level <2504000 2950000>
                  set_bytes("/soc/ufshc@1d84000", "vcc-voltage-level", std::move(vl)); }

                // AVB chain: the real vbmeta_a chains to vbmeta_system + vbmeta_vendor.
                // The stock DT /firmware/android/vbmeta parts list ("vbmeta,boot,
                // system,vendor,dtbo") omits them, so first-stage init never creates
                // their by-name symlinks and avb_slot_verify fails with "Device path
                // not found: /dev/block/by-name/vbmeta_system_a" (result 2). Add the
                // chained vbmeta partitions so init sets them up (data is real, from
                // the OTA).
                set_str("/firmware/android/vbmeta", "parts",
                        "vbmeta,boot,system,vendor,dtbo,vbmeta_system,vbmeta_vendor");

                // Dynamic partitions: the stock DT fstab lists only a static
                // /vendor, but this build uses dynamic partitions (system/vendor/
                // etc. are logical inside `super`, per /first_stage_ramdisk/
                // fstab.hollywood). Neutralize the DT fstab's compatible so
                // first-stage init falls back to that ramdisk fstab and reads the
                // liblp metadata from super.
                set_str("/firmware/android/fstab", "compatible", "disabled");
            }
            // Verify the initrd properties survived in the final blob.
            try {
                auto f2 = boot::Fdt::parse(emu_.dtb_image);
                if (const boot::FdtNode* ch = f2.find("/chosen")) {
                    for (const auto& p : ch->props)
                        HW_WARN("boot.dtb", "  /chosen prop: {} ({} bytes)", p.name, p.data.size());
                } else HW_WARN("boot.dtb", "  /chosen NOT FOUND in final blob!");
            } catch (const std::exception& e) { HW_WARN("boot.dtb", "  final blob re-parse FAILED: {}", e.what()); }
            emu_.ram->load(emu_.dtb_load, emu_.dtb_image);
            cursor = round_up(emu_.dtb_load + emu_.dtb_image.size(), 0x100000);
        }
        if (!emu_.boot_img.ramdisk.empty()) {
            if (!emu_.ramdisk_load) emu_.ramdisk_load = cursor;   // no-DTB fallback
            emu_.ram->load(emu_.ramdisk_load, emu_.boot_img.ramdisk);
        }
        return std::format("{} MB @ {:#x}; kernel@{:#x} dtb@{:#x} ramdisk@{:#x}",
                           emu_.config.ram_mb, emu_.dram_base, emu_.kernel_load,
                           emu_.dtb_load, emu_.ramdisk_load);
    });

    if (!failed_) dump_memory_map();   // print the physical topology + overlap check

    // Phase 11: cold-boot the REAL Qualcomm secure monitor (tz) at EL3 in the full
    // machine, instead of handing off to the Linux kernel. The machine (RAM +
    // device models: GCC/GIC/RNG/UFS/RSC/UART) is already built above; we attach
    // the EL3-enabled backend, load tz into its secure carveouts, and run it.
    if (emu_.config.tz_boot && !failed_) {
        stage("Attaching ARM64 execution backend (tz / EL3)", [&] {
            std::string err;
            if (!emu_.backend->attach(*emu_.ram, emu_.bus, err))
                throw std::runtime_error(err);
            return std::string(emu_.backend->name());
        });
        if (failed_) { print_diagnostics("Attaching ARM64 execution backend (tz / EL3)"); return 1; }
        return run_tz(*zip, payload);
    }

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
            dump_kmsg();   // show boot progress even on a clean halt/timeout
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

int BootPipeline::run_tz(ota::ZipReader& zip, ota::Payload& payload) {
    cpu::CpuBackend* be = emu_.backend.get();
    uint64_t tz_entry = 0;

    // Load the real `tz` ELF into its secure physical carveouts. tz is a Qualcomm
    // MI multi-segment ELF: load every PT_LOAD with memsz>0 (the NULL header/hash
    // segments are skipped), coalescing page ranges into mappable regions.
    stage("Loading secure monitor (tz)", [&] {
        Bytes tz = ota::extract_partition(zip, payload, "tz");
        if (tz.size() < 64 || std::memcmp(tz.data(), "\x7f""ELF", 4) != 0)
            throw std::runtime_error("tz partition is not an ELF");
        auto rd64 = [&](size_t o){ uint64_t v; std::memcpy(&v, tz.data() + o, 8); return v; };
        auto rd16 = [&](size_t o){ uint16_t v; std::memcpy(&v, tz.data() + o, 2); return v; };
        tz_entry = rd64(24);
        uint64_t phoff = rd64(32);
        uint16_t phentsize = rd16(54), phnum = rd16(56);

        struct Seg { uint64_t off, paddr, filesz, memsz; };
        std::vector<Seg> segs;
        for (int i = 0; i < phnum; i++) {
            const uint8_t* p = tz.data() + phoff + (uint64_t)i * phentsize;
            uint32_t type; std::memcpy(&type, p, 4);
            Seg s;
            std::memcpy(&s.off,    p + 8,  8);
            std::memcpy(&s.paddr,  p + 24, 8);
            std::memcpy(&s.filesz, p + 32, 8);
            std::memcpy(&s.memsz,  p + 40, 8);
            if (type == 1 /*PT_LOAD*/ && s.memsz > 0) segs.push_back(s);
        }
        if (segs.empty()) throw std::runtime_error("tz has no loadable segments");

        struct Rgn { uint64_t lo, hi; };
        std::vector<Rgn> raw;
        for (auto& s : segs)
            raw.push_back({ s.paddr & ~0xfffull, (s.paddr + s.memsz + 0xfff) & ~0xfffull });
        std::sort(raw.begin(), raw.end(), [](const Rgn& a, const Rgn& b){ return a.lo < b.lo; });
        std::vector<Rgn> merged;
        for (auto& r : raw) {
            if (!merged.empty() && r.lo <= merged.back().hi) {
                if (r.hi > merged.back().hi) merged.back().hi = r.hi;
            } else merged.push_back(r);
        }
        std::string err;
        for (auto& r : merged)
            if (!be->map_ram_region(r.lo, r.hi - r.lo, err))
                throw std::runtime_error("map tz region: " + err);
        for (auto& s : segs)
            be->write_phys(s.paddr, tz.data() + s.off, (size_t)s.filesz);
        // Scratch region backing the xbl->tz boot-handoff params (x0/x1 must be
        // nonzero; tz stashes them and parks its cold boot if either is zero).
        if (!be->map_ram_region(0x14700000ull, 0x20000ull, err))
            throw std::runtime_error("map tz boot-param scratch: " + err);

        return std::format("tz {} KB, entry {:#x}, {} PT_LOAD segs -> {} region(s)",
                           tz.size() / 1024, tz_entry, segs.size(), merged.size());
    });
    if (failed_) { print_diagnostics("Loading secure monitor (tz)"); return 1; }

    // Phase 1: cold-boot tz at EL3 until it hands off (ERETs to a lower EL). x0/x1
    // are the nonzero boot-handoff params (tz parks if either is zero).
    emu_.cpu.setup_tz_boot(tz_entry, 0x14700000ull, 0x14710000ull);
    be->set_state(emu_.cpu);
    std::printf("[%s....%s] Cold-booting real secure monitor (tz) at EL3\n", YELL, RST);
    std::fflush(stdout);
    cpu::RunResult rr = be->run(emu_.config.max_instructions);

    if (!be->tz_dropped()) {
        // tz never reached its non-secure handoff -- report where it stalled.
        std::printf("\n[%stz%s] did NOT reach handoff: %s%s%s  (%llu insns, PC=%#llx)\n",
                    BOLD, RST, RED, rr.detail.c_str(), RST,
                    (unsigned long long)rr.instructions_executed, (unsigned long long)rr.pc);
        failed_ = true;
        print_arm64_exception(rr);
        return 1;
    }
    std::printf("[ %sOK%s ] tz cold boot complete -- ERET to lower EL (would-be target %#llx, %llu insns at EL3)\n",
                GREEN, RST, (unsigned long long)be->tz_drop_pc(),
                (unsigned long long)rr.instructions_executed);

    // Phase 2: inject the kernel boot handoff. tz stays resident at EL3 (its
    // VBAR_EL3 SMC handler + secure-world setup are preserved -- set_state only
    // rewrites x0..x30/SP/PC/PSTATE). The kernel runs at EL1 and its qcom_scm /
    // PSCI SMCs now trap to the REAL tz instead of QEMU's PSCI stub.
    emu_.cpu.setup_linux_boot(emu_.kernel_load, emu_.dtb_load);
    be->set_state(emu_.cpu);
    std::printf("[%s....%s] Kernel handoff (EL1) with tz resident at EL3  PC=%#llx x0(dtb)=%#llx\n",
                YELL, RST, (unsigned long long)emu_.kernel_load, (unsigned long long)emu_.dtb_load);
    std::fflush(stdout);
    cpu::RunResult rr2 = be->run(emu_.config.max_instructions);
    std::printf("\n[%skernel+tz%s] stopped: %s%s%s  (%llu insns, PC=%#llx)\n",
                BOLD, RST, DIM, rr2.detail.c_str(), RST,
                (unsigned long long)rr2.instructions_executed, (unsigned long long)rr2.pc);
    if (!rr2.ok()) { failed_ = true; print_arm64_exception(rr2); }
    dump_kmsg();
    print_diagnostics("");
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
        // Collapse runs of the same PC (e.g. an undeliverable-exception storm) so
        // the real trajectory leading to the stop stays visible.
        std::printf("\nRecent instruction trace (oldest first, consecutive repeats collapsed):\n");
        uint64_t prev = ~0ull; uint64_t reps = 0; size_t shown = 0;
        auto flush = [&](){ if (reps > 1) std::printf("      ... x%llu\n", (unsigned long long)reps); };
        for (size_t i = 0; i < pcs.size() && shown < 80; ++i) {
            if (pcs[i] == prev) { reps++; continue; }
            flush();
            std::printf("  %#018llx: %s\n", (unsigned long long)pcs[i],
                        emu_.backend->disasm_at(pcs[i]).c_str());
            prev = pcs[i]; reps = 1; shown++;
        }
        flush();
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
void BootPipeline::list_dt() {
    if (!emu_.fdt || !emu_.fdt->root()) { std::printf("(no DTB)\n"); return; }
    std::printf("\n%s== device-tree node survey (path : compatible) ==%s\n", BOLD, RST);
    int count = 0;
    auto walk = [&](const boot::FdtNode* n, auto&& self) -> void {
        std::string path = boot::fdt_node_path(n);
        std::string comp = n->compatible();
        const boot::FdtProp* st = n->prop("status");
        std::printf("  %-52s %s%s\n", path.c_str(), comp.empty() ? "-" : comp.c_str(),
                    st ? (std::string("  [status=") + st->str() + "]").c_str() : "");
        count++;
        for (auto& c : n->children) self(c.get(), self);
    };
    walk(emu_.fdt->root(), walk);
    std::printf("%s== %d nodes ==%s\n\n", BOLD, count, RST);
}

// Apply the minimal-profile node disables (config.dtb_disable) to the DTB the
// kernel will actually boot, leaving the stock blob (emu_.boot_img.dtb)
// untouched. Every modification is logged. The stock and resulting minimal DTB
// are written to profiles/ so the faithful config is always recoverable.
void BootPipeline::apply_dtb_profile(Bytes& dtb) {
    namespace fs = std::filesystem;
    // Always preserve the stock DTB on disk.
    try {
        fs::create_directories("profiles/quest2-stock");
        std::FILE* f = std::fopen("profiles/quest2-stock/quest2.dtb", "wb");
        if (f) { std::fwrite(emu_.boot_img.dtb.data(), 1, emu_.boot_img.dtb.size(), f); std::fclose(f); }
    } catch (...) {}

    if (emu_.config.dtb_disable.empty()) return;   // stock profile: no changes

    HW_WARN("dtb", "[{}] applying {} node disable(s):", emu_.config.profile,
            emu_.config.dtb_disable.size());
    for (const auto& id : emu_.config.dtb_disable) {
        bool modified = false; std::string path;
        Bytes nb = boot::fdt_disable(dtb, id, modified, path);
        if (modified) { dtb = std::move(nb);
            HW_WARN("dtb", "  disabled {}  ({})", path, id);
        } else {
            HW_WARN("dtb", "  NOT FOUND: {} (no matching node -- skipped)", id);
        }
    }
    // Save the resulting minimal DTB for the record.
    try {
        fs::create_directories("profiles/quest2-minimal");
        std::FILE* f = std::fopen("profiles/quest2-minimal/quest2.dtb", "wb");
        if (f) { std::fwrite(dtb.data(), 1, dtb.size(), f); std::fclose(f); }
    } catch (...) {}
}

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

// Print the guest physical memory topology as Linux will see it: the /memory
// window, every /reserved-memory child (base/size/no-map/reusable), and where WE
// placed the kernel image (footprint = image_size), DTB and initramfs. Flags any
// overlap between a loaded image and a reserved region -- the prime suspect for
// buddy-allocator / struct-page corruption.
uint64_t BootPipeline::find_free_load_base(uint64_t total_size, uint64_t align) {
    struct R { uint64_t base, end; };
    std::vector<R> res;
    if (emu_.fdt) if (const boot::FdtNode* rm = emu_.fdt->find("/reserved-memory")) {
        for (const auto& c : rm->children)
            for (auto& pr : c->reg())
                if (pr.second) res.push_back({ pr.first, pr.first + pr.second });
    }
    std::sort(res.begin(), res.end(), [](const R& a, const R& b){ return a.base < b.base; });
    const uint64_t ram_lo = emu_.dram_base, ram_hi = emu_.dram_base + emu_.ram_size;
    uint64_t cand = round_up(ram_lo, align);
    for (;;) {
        bool moved = false;
        for (auto& r : res)
            if (cand < r.end && r.base < cand + total_size) {   // [cand,cand+size) hits r
                cand = round_up(r.end, align);
                moved = true;
            }
        if (cand + total_size > ram_hi) return 0;               // no room
        if (!moved) return cand;                                // clean run found
    }
}

void BootPipeline::dump_memory_map() {
    if (!emu_.fdt || !emu_.fdt->root()) return;
    std::printf("\n%s== GUEST PHYSICAL MEMORY MAP ==%s\n", BOLD, RST);
    const uint64_t ram_lo = emu_.dram_base, ram_hi = emu_.dram_base + emu_.ram_size;
    std::printf("  /memory (patched): %#llx .. %#llx  (%llu MB)\n",
                (unsigned long long)ram_lo, (unsigned long long)ram_hi,
                (unsigned long long)(emu_.ram_size / (1024 * 1024)));

    // Our loaded images (as physical spans).
    struct Span { const char* name; uint64_t base, end; };
    std::vector<Span> ours;
    uint64_t kimg = std::max<uint64_t>(emu_.boot_img.kernel.size(), emu_.boot_img.kernel_image_size);
    ours.push_back({ "kernel(image_size)", emu_.kernel_load, emu_.kernel_load + kimg });
    if (!emu_.dtb_image.empty()) ours.push_back({ "dtb", emu_.dtb_load, emu_.dtb_load + emu_.dtb_image.size() });
    if (emu_.ramdisk_load) ours.push_back({ "initramfs", emu_.ramdisk_load, emu_.ramdisk_load + emu_.boot_img.ramdisk.size() });
    std::printf("  %sour image placement:%s\n", BOLD, RST);
    for (auto& s : ours)
        std::printf("    %-18s %#llx .. %#llx  (%llu KB)\n", s.name,
                    (unsigned long long)s.base, (unsigned long long)s.end,
                    (unsigned long long)((s.end - s.base) / 1024));

    // Reserved-memory children.
    const boot::FdtNode* rm = emu_.fdt->find("/reserved-memory");
    if (!rm) { std::printf("  (no /reserved-memory node)\n"); return; }
    std::printf("  %s/reserved-memory regions:%s\n", BOLD, RST);
    std::vector<Span> resv;
    for (const auto& c : rm->children) {
        auto regs = c->reg();
        bool nomap = c->prop("no-map") != nullptr;
        bool reusable = c->prop("reusable") != nullptr;
        const boot::FdtProp* sz = c->prop("size");     // dynamic-placement CMA (no fixed reg)
        if (regs.empty() && sz) {
            std::printf("    %-34s DYNAMIC size=%#llx %s%s\n", c->name.c_str(),
                        (unsigned long long)(sz->data.size() >= 8 ? sz->u64() : sz->u32()),
                        nomap ? "no-map " : "", reusable ? "reusable" : "");
            continue;
        }
        for (auto& [base, size] : regs) {
            bool in_ram = (base >= ram_lo && base < ram_hi);
            std::printf("    %-34s %#llx .. %#llx (%llu KB) %s%s%s\n", c->name.c_str(),
                        (unsigned long long)base, (unsigned long long)(base + size),
                        (unsigned long long)(size / 1024),
                        nomap ? "no-map " : "", reusable ? "reusable " : "",
                        in_ram ? "" : "[OUTSIDE /memory]");
            if (size) resv.push_back({ nomap ? "no-map" : "reserved", base, base + size });
        }
    }

    // Overlap check: any of our loaded images vs any reserved region.
    std::printf("  %soverlap check (loaded image vs reserved region):%s\n", BOLD, RST);
    bool any = false;
    for (auto& o : ours)
        for (auto& r : resv)
            if (o.base < r.end && r.base < o.end) {
                any = true;
                std::printf("    %s*** OVERLAP: %s [%#llx..%#llx] intersects %s [%#llx..%#llx] ***%s\n",
                            RED, o.name, (unsigned long long)o.base, (unsigned long long)o.end,
                            r.name, (unsigned long long)r.base, (unsigned long long)r.end, RST);
            }
    if (!any) std::printf("    none\n");
    std::printf("\n");
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

// Walk the printk ring buffer using the kernel's own indices (reliable even
// after the buffer wraps). Symbol VAs are fixed for the Quest 2 4.19 kernel
// (extract with tools/ksyms.cpp; PA = VA - 0xffffff8008080000 + 0x80080000).
// Returns true if it produced a coherent log.
bool BootPipeline::dump_kmsg_symbols() {
    if (!emu_.backend) return false;
    constexpr uint64_t VA_log_buf      = 0xffffff8009be4670ull; // char *log_buf
    constexpr uint64_t VA_log_buf_len  = 0xffffff8009be4678ull; // u32
    constexpr uint64_t VA_log_first_idx= 0xffffff8009da03b8ull; // u32
    constexpr uint64_t VA_log_next_idx = 0xffffff8009da0398ull; // u32
    uint64_t buf_va = 0; uint32_t buf_len = 0, first = 0, next = 0;
    if (!emu_.backend->read_mem(VA_log_buf, &buf_va, 8)) return false;
    if (!emu_.backend->read_mem(VA_log_buf_len, &buf_len, 4)) return false;
    if (!emu_.backend->read_mem(VA_log_first_idx, &first, 4)) return false;
    if (!emu_.backend->read_mem(VA_log_next_idx, &next, 4)) return false;
    if (buf_va < 0xffffff8000000000ull || buf_len < 0x1000 || buf_len > 0x400000) return false;
    if (first >= buf_len || next >= buf_len) return false;

    std::printf("\n%s== kernel log (printk ring @ %#llx, len %u, first=%u next=%u) ==%s\n",
                BOLD, (unsigned long long)buf_va, buf_len, first, next, RST);
    uint32_t idx = first; int printed = 0;
    for (int n = 0; n < 60000; ++n) {
        if (idx == next) break;
        uint8_t hdr[16];
        if (!emu_.backend->read_mem(buf_va + idx, hdr, 16)) break;
        uint16_t len = (uint16_t)(hdr[8] | (hdr[9] << 8));
        uint16_t text_len = (uint16_t)(hdr[10] | (hdr[11] << 8));
        if (len == 0) { idx = 0; continue; }                 // wrapped to start
        if (idx + 16 + text_len > buf_len) break;
        if (text_len > 0 && text_len <= 1024) {
            std::string line(text_len, '\0');
            if (emu_.backend->read_mem(buf_va + idx + 16, line.data(), text_len)) {
                std::printf("  %s\n", line.c_str()); printed++;
            }
        }
        idx += len;
    }
    std::printf("%s== end kernel log (%d lines) ==%s\n", BOLD, printed, RST);
    return printed > 0;
}

void BootPipeline::dump_kmsg() {
    if (dump_kmsg_symbols()) return;   // reliable index-based walk first
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
