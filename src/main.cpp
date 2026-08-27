// hollywood_emu -- Windows-hosted Meta Quest 2 (hollywood / Snapdragon XR2) emulator.
//
// Subcommands:
//   boot     Run the full boot pipeline against the OTA (default).
//   inspect  Print the OTA partition table.
//   extract  Extract a single partition from the OTA to a file.
#include "core/boot_pipeline.h"
#include "core/emulator.h"
#include "cpu/unicorn_cpu.h"
#include "cpu/cpu_state.h"
#include "memory/guest_memory.h"
#include "devices/device_bus.h"
#include "devices/stub_device.h"
#include "common/log.h"
#include "ota/payload.h"
#include "ota/zip_reader.h"

#include <memory>
#include <span>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace hw;

namespace {

// Defaults point at the artifacts supplied for bring-up.
constexpr const char* kDefaultOta =
    R"(C:\Users\drych\Downloads\q2_52242990021400150.zip)";
constexpr const char* kDefaultKernel =
    R"(C:\Users\drych\Downloads\oculus-linux-kernel-oculus-quest2-kernel-master.zip)";

struct Args {
    std::string cmd = "boot";
    std::string ota = kDefaultOta;
    std::string kernel = kDefaultKernel;
    std::string part = "boot";
    std::string out;
    uint64_t ram_mb = 2048;
    uint64_t max_insns = 200000000ull;
    uint64_t timeout_s = 20;
    uint64_t trace_limit = 300;
    bool verbose = false;
    bool trace = false;
    bool stop_on_mmio = true;
    bool log_mmio = false;
    bool no_hooks = false;
    bool owned_mem = false;
    bool step = false;
    bool cpu_tlb = false;   // use Unicorn's native CPU TLB (native MMU + native exceptions)
    bool no_spin = false;   // disable spin-wait detection (for slow but progressing loops)
    bool trace_user = false; // dump exec->EL0 handoff + trace userspace ABI (syscalls/faults)
    uint64_t trace_user_insns = 20000;
    bool stop_on_undef = false;
    bool trace_irq = false; // symbol-trace timer/irq functions (needs build/ksyms.txt)
    bool dump_dt = false;
    bool list_dt = false;   // print every DT node path + compatible and exit
    bool tz = false;        // cold-boot the real Qualcomm secure monitor (tz) at EL3
    bool kmshim = false;    // NON-FAITHFUL keymaster/QSEE SCM shim (boot past real QSEE)
    std::string profile = "stock";          // "stock" or "minimal"
    std::vector<std::string> disable_nodes; // extra DT nodes to disable (compatible/path)
};

const char* arg_val(int argc, char** argv, int& i) {
    if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); std::exit(2); }
    return argv[++i];
}

void usage() {
    std::printf(
        "hollywood_emu -- Meta Quest 2 emulator\n\n"
        "Usage:\n"
        "  hollywood_emu [boot]    [--ota <zip>] [--ram <MB>] [exec options]\n"
        "  hollywood_emu inspect   [--ota <zip>]\n"
        "  hollywood_emu extract    --part <name> [--out <file>] [--ota <zip>]\n"
        "  hollywood_emu selftest  (validate the ARM64 CPU backend in isolation)\n\n"
        "Options:\n"
        "  --ota <zip>            Quest 2 OTA image (payload.bin container)\n"
        "  --kernel <zip>         Quest 2 kernel source archive (reference)\n"
        "  --ram <MB>             Guest RAM size (default 2048)\n"
        "  --part <name>          Partition to extract (boot, dtbo, vendor, ...)\n"
        "  --out <file>           Output path for extract\n"
        "  -v, --verbose          Verbose subsystem logging\n\n"
        "Execution options (boot):\n"
        "  --trace                Trace the first N executed instructions (disasm)\n"
        "  --max-instructions N   Cap executed instructions (default 200000000)\n"
        "  --timeout N            Wall-clock cap in seconds (default 20, 0=off)\n"
        "  --stop-on-mmio         Halt on the first unclaimed access (default)\n"
        "  --permissive           Back unknown accesses with zero RAM and continue\n"
        "  --log-mmio             Log every device MMIO access\n"
        "  --debug                Verbose logging + MMIO logging\n");
}

int cmd_inspect(const Args& a) {
    ota::ZipReader zip(a.ota);
    ota::Payload pl = ota::parse_payload(zip);
    std::printf("\nOTA: %s\n", a.ota.c_str());
    std::printf("payload v%llu  block=%u  %zu partitions\n\n",
                (unsigned long long)pl.version, pl.block_size, pl.partitions.size());
    std::printf("%-16s %12s %6s  ops\n", "PARTITION", "SIZE(MB)", "OPS");
    std::printf("--------------------------------------------------------\n");
    for (const auto& p : pl.partitions) {
        std::printf("%-16s %12.2f %6zu\n", p.name.c_str(),
                    p.size / 1048576.0, p.ops.size());
    }
    std::printf("\ndynamic_partition_metadata: %s, %zu group(s)\n",
                pl.dap.present ? "present" : "ABSENT", pl.dap.groups.size());
    for (const auto& g : pl.dap.groups) {
        std::printf("  group '%s' max=%.2f MB  partitions: ", g.name.c_str(), g.maximum_size / 1048576.0);
        for (const auto& n : g.partition_names) std::printf("%s ", n.c_str());
        std::printf("\n");
    }
    return 0;
}

int cmd_extract(const Args& a) {
    ota::ZipReader zip(a.ota);
    ota::Payload pl = ota::parse_payload(zip);
    Bytes data = ota::extract_partition(zip, pl, a.part);
    std::string out = a.out.empty() ? (a.part + ".img") : a.out;
    fs::create_directories(fs::path(out).parent_path().empty() ? "." : fs::path(out).parent_path());
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), data.size());
    return 0;
}

// Synthetic ARM64 payload to validate the CPU backend independently of the
// Quest 2 kernel (so kernel-specific boot issues can't be mistaken for backend
// bugs). Tests: init, RAM mapping, register/PC/DTB init, real instruction
// execution, MMIO interception, instruction limit, and memory read-back.
int cmd_selftest(const Args&) {
    std::printf("\nhollywood_emu -- CPU backend self-test\n\n");
    int failures = 0;
    auto check = [&](const char* n, bool ok) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", n);
        if (!ok) failures++;
    };

    mem::GuestMemory ram(0x80000000ull, 0x100000);          // 1 MB
    dev::DeviceBus bus;
    auto stub = std::make_unique<dev::StubDevice>("test_mmio", 0x09000000ull, 0x1000);
    dev::StubDevice* stub_ptr = stub.get();
    bus.add(std::move(stub));

    // mov x0,#1 ; mov x1,#2 ; add x2,x0,x1 ; movz x3,#0x900,lsl#16 ;
    // str w2,[x3] ; ldr w4,[x3] ; b .
    const uint32_t prog[] = {
        0xd2800020, 0xd2800041, 0x8b010002, 0xd2a12003,
        0xb9000062, 0xb9400064, 0x14000000,
    };
    ram.load(0x80000000ull, std::span<const uint8_t>(
                 reinterpret_cast<const uint8_t*>(prog), sizeof(prog)));

    cpu::CpuState st;
    st.setup_linux_boot(0x80000000ull, 0x80080000ull);      // pc=entry, x0=dtb

    cpu::UnicornOptions uo;
    uo.stop_on_unmapped = false; uo.timeout_us = 2000000; uo.heartbeat = 0; uo.hot_threshold = 100000;
    cpu::UnicornCpu backend(uo);

    std::string err;
    check("attach: create CPU, map RAM + MMIO", backend.attach(ram, bus, err));
    if (!err.empty()) std::printf("      err: %s\n", err.c_str());
    backend.set_state(st);

    cpu::Aarch64Regs pre = backend.read_regs();
    check("PC initialized to kernel entry", pre.pc == 0x80000000ull);
    check("X0 initialized to DTB pointer", pre.x[0] == 0x80080000ull);

    std::string dis = backend.disasm_at(0x80000000ull);
    std::printf("      disasm[entry]: %s\n", dis.c_str());
    check("disassembler available (capstone)", dis.rfind(".word", 0) != 0 && dis != "<unreadable>");

    cpu::RunResult rr = backend.run(6);                     // stop before the spin branch
    cpu::Aarch64Regs post = backend.read_regs();
    check("executed real ARM64 (add -> x2 == 3)", post.x[2] == 3);
    check("MMIO write routed to device", stub_ptr->writes() == 1);
    check("MMIO read routed to device", stub_ptr->reads() == 1);
    check("instruction limit honored (>=5 insns)", rr.instructions_executed >= 5);
    uint8_t back[4] = {};
    bool memok = backend.read_mem(0x80000000ull, back, 4) &&
                 (*reinterpret_cast<uint32_t*>(back) == 0xd2800020u);
    check("guest RAM visible through backend", memok);

    std::printf("\n%s (%d failure%s)\n\n",
                failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

int cmd_boot(const Args& a) {
    core::EmuConfig cfg;
    cfg.ota_zip = a.ota;
    cfg.kernel_zip = a.kernel;
    cfg.ram_mb = a.ram_mb;
    cfg.verbose = a.verbose;
    cfg.max_instructions = a.max_insns;
    cfg.trace = a.trace;
    cfg.stop_on_mmio = a.stop_on_mmio;
    cfg.log_mmio = a.log_mmio;
    cfg.dump_dt = a.dump_dt;
    cfg.list_dt = a.list_dt;
    cfg.tz_boot = a.tz;
    cfg.profile = a.profile;
    // The "minimal" profile disables non-essential vendor devices (display, camera,
    // GPU, sensors) so the kernel reaches userspace/initramfs. Nodes are named by a
    // `compatible` substring; each is verified (found/skipped) and logged at boot.
    // This list is built up iteratively from real probe failures -- it starts with
    // the display stack (the current fatal SDE crash) and grows per attempt.
    if (a.profile == "minimal") {
        cfg.dtb_disable = {
            // Display subsystem (SDE/MDSS/DSI) -- non-essential for a headless
            // console boot, depends on unemulated display clocks/regulators, and
            // its probe/cleanup paths crash/livelock (sde_rsc NULL-deref @489M,
            // then devres_remove loop in sde-kms cleanup).
            "/soc/qcom,sde_rscc@af20000",        // qcom,sde-rsc
            "/soc/qcom,mdss_mdp@ae00000",        // qcom,sde-kms (display core)
            "/soc/qcom,mdss_dsi_ctrl0@ae94000",  // qcom,dsi-ctrl-hw-v2.4
            "/soc/qcom,mdss_dsi_ctrl1@ae96000",  // qcom,dsi-ctrl-hw-v2.4
            "/soc/qcom,mdss_dsi_phy0@ae94400",   // qcom,dsi-phy-v4.1
            "/soc/qcom,mdss_dsi_phy1@ae96400",   // qcom,dsi-phy-v4.1
            "/soc/qcom,mdss_dsi_pll@ae94900",    // mdss_pll (Bad page state @642M)
            "/soc/qcom,mdss_dsi_pll@ae96900",    // mdss_pll
            // Camera subsystem (CAM CPAS/CDM/SMMU/ISP/ICP/JPEG/CCI) -- non-essential
            // for console boot. cam-cpas fails request_platform_resource (-517), then
            // the cam-cdm probe calls cam_smmu_get_handle() which strcmp()s a NULL
            // context-bank name -> kernel NULL deref in swapper/0 (initcall) -> panic
            // "Attempted to kill init". Depends on unemulated camera clocks/CPAS HW.
            "/soc/qcom,cam-cpas@ac40000",        // qcom,cam-cpas (resource-alloc failure root)
            "/soc/qcom,cam-cdm-intf",            // qcom,cam-cdm-intf
            "/soc/qcom,cpas-cdm0@ac4d000",       // qcom,cam170-cpas-cdm0 (probe NULL-derefs)
            "/soc/qcom,cam_smmu",                // qcom,msm-cam-smmu (empty CB list -> NULL name)
            "/soc/qcom,cam-isp",                 // qcom,cam-isp
            "/soc/qcom,cam-icp",                 // qcom,cam-icp
            "/soc/qcom,cam-a5@ac00000",          // qcom,cam-a5 (ICP firmware core)
            "/soc/qcom,cam-jpeg",                // qcom,cam-jpeg
            "/soc/qcom,jpegenc@ac53000",         // qcom,cam_jpeg_enc
            "/soc/qcom,jpegdma@ac57000",         // qcom,cam_jpeg_dma
            "/soc/qcom,cam-bps",                 // qcom,cam-bps
            "/soc/qcom,ipe0",                    // qcom,cam-ipe
            "/soc/qcom,cam-req-mgr",             // qcom,cam-req-mgr
            "/soc/qcom,cci@ac4f000",             // qcom,cci
            "/soc/qcom,cci@ac50000",             // qcom,cci
        };
    }
    for (const auto& d : a.disable_nodes) cfg.dtb_disable.push_back(d);
    core::Emulator emu(std::move(cfg));

    // Attach the real ARM64 execution backend (Unicorn / QEMU-TCG).
    cpu::UnicornOptions uopts;
    uopts.trace = emu.config.trace;
    uopts.stop_on_unmapped = emu.config.stop_on_mmio;
    uopts.log_mmio = emu.config.log_mmio;
    uopts.timeout_us = a.timeout_s * 1000000ull;
    uopts.trace_limit = a.trace_limit;
    uopts.code_hook = !a.no_hooks;
    uopts.host_backed_ram = !a.owned_mem;
    uopts.step = a.step;
    if (a.cpu_tlb) { uopts.our_mmu = false; uopts.vector_exc = false; }  // native MMU + exceptions
    uopts.stop_on_undef = a.stop_on_undef;
    if (a.trace_irq) uopts.fn_trace_ksyms = "build/ksyms.txt";
    if (a.no_spin) uopts.hot_threshold = 0;   // disable spin detection (rely on timeout)
    uopts.trace_user = a.trace_user;
    uopts.trace_user_insns = a.trace_user_insns;
    uopts.el3 = a.tz;   // enable EL3 secure monitor when cold-booting tz
    uopts.kmshim = a.kmshim;   // NON-FAITHFUL keymaster/QSEE SCM shim
    emu.backend = std::make_unique<cpu::UnicornCpu>(uopts);

    core::BootPipeline pipeline(emu);
    return pipeline.run();
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: capture output up to any crash
#ifdef _WIN32
    // Enable ANSI escape processing on the Windows console.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    Args a;
    int start = 1;
    if (argc > 1 && argv[1][0] != '-') { a.cmd = argv[1]; start = 2; }

    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--ota") a.ota = arg_val(argc, argv, i);
        else if (s == "--kernel") a.kernel = arg_val(argc, argv, i);
        else if (s == "--part") a.part = arg_val(argc, argv, i);
        else if (s == "--out") a.out = arg_val(argc, argv, i);
        else if (s == "--ram") a.ram_mb = std::strtoull(arg_val(argc, argv, i), nullptr, 10);
        else if (s == "--max-instructions") a.max_insns = std::strtoull(arg_val(argc, argv, i), nullptr, 10);
        else if (s == "--timeout") a.timeout_s = std::strtoull(arg_val(argc, argv, i), nullptr, 10);
        else if (s == "--trace") a.trace = true;
        else if (s == "--trace-limit") a.trace_limit = std::strtoull(arg_val(argc, argv, i), nullptr, 10);
        else if (s == "--stop-on-mmio") a.stop_on_mmio = true;
        else if (s == "--permissive" || s == "--no-stop-on-mmio") a.stop_on_mmio = false;
        else if (s == "--log-mmio") a.log_mmio = true;
        else if (s == "--no-hooks") a.no_hooks = true;
        else if (s == "--owned-mem") a.owned_mem = true;
        else if (s == "--step") a.step = true;
        else if (s == "--cpu-tlb") a.cpu_tlb = true;
        else if (s == "--no-spin") a.no_spin = true;
        else if (s == "--trace-user") a.trace_user = true;
        else if (s == "--trace-user-insns") a.trace_user_insns = std::strtoull(arg_val(argc, argv, i), nullptr, 10);
        else if (s == "--stop-on-undef") a.stop_on_undef = true;
        else if (s == "--trace-irq") a.trace_irq = true;
        else if (s == "--dump-dt") a.dump_dt = true;
        else if (s == "--list-dt") a.list_dt = true;
        else if (s == "--tz") a.tz = true;
        else if (s == "--kmshim") a.kmshim = true;
        else if (s == "--profile") a.profile = arg_val(argc, argv, i);
        else if (s == "--disable-node") a.disable_nodes.push_back(arg_val(argc, argv, i));
        else if (s == "--debug") { a.verbose = true; a.log_mmio = true; }
        else if (s == "-v" || s == "--verbose") a.verbose = true;
        else if (s == "-h" || s == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); usage(); return 2; }
    }

    try {
        if (a.cmd == "boot")     return cmd_boot(a);
        if (a.cmd == "inspect")  return cmd_inspect(a);
        if (a.cmd == "extract")  return cmd_extract(a);
        if (a.cmd == "selftest") return cmd_selftest(a);
        std::fprintf(stderr, "unknown command: %s\n", a.cmd.c_str());
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nfatal: %s\n", e.what());
        return 1;
    }
}
