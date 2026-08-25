# hollywood_emu

A Windows-hosted emulator for the **Meta Quest 2** operating system, built by
reverse-engineering the supplied Quest 2 OTA image and Linux kernel source.
Device codename **`hollywood`**; SoC is a Qualcomm **Snapdragon XR2 ("Kona" /
SM8250)**; the OS is **Android 14** on **Linux 4.19.325**.

The emulator boots the *unaltered* Quest 2 kernel: it extracts the real kernel
from the OTA, parses the boot image and device tree, lays out guest memory, and
executes real ARM64 instructions on a Unicorn (QEMU-TCG) core.

## Building

Requires Visual Studio 2026 (MSVC + Windows SDK) with the bundled CMake and
vcpkg. Dependencies (`liblzma`, `bzip2`, `zlib`, `unicorn`, `capstone[arm64]`)
are pulled automatically via the vcpkg manifest (`vcpkg.json`).

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$tc    = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg\scripts\buildsystems\vcpkg.cmake"
& $cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE="$tc"
& $cmake --build build --config Release
```

Output: `build\Release\hollywood_emu.exe`.

## Usage

```
hollywood_emu boot      [--ota <zip>] [--ram <MB>] [exec options]
hollywood_emu inspect   [--ota <zip>]        # print the OTA partition table
hollywood_emu extract    --part <name>       # extract a partition (boot, dtbo, ...)
hollywood_emu selftest                       # validate the ARM64 CPU backend

Execution options:
  --trace [--trace-limit N]   disassembled instruction trace
  --max-instructions N        cap executed instructions
  --timeout N                 wall-clock cap (seconds)
  --stop-on-mmio | --permissive   halt vs. zero-fill on unclaimed access
  --log-mmio | --debug        MMIO / verbose logging
  --no-hooks | --owned-mem | --step   backend diagnostics
```

The OTA and kernel-source paths default to the supplied artifacts.

## Architecture

```
src/
  common/    logging, byte readers, XZ/BZ2/gzip decompression
  ota/       ZIP reader + Android update_engine payload.bin parser/extractor
  boot/      Android boot-image parser, flattened device-tree (FDT) parser
  memory/    guest RAM (single host-backed buffer)
  cpu/        ARM64 CpuState + boot protocol; CpuBackend iface; Unicorn backend
  devices/    MMIO device bus; GENI UART console; stub devices
  platform/   Kona SoC memory map
  core/       Emulator container + staged boot pipeline & diagnostics
```

The `CpuBackend` interface deliberately decouples the machine (memory, devices,
loader) from the execution core, so the core can be swapped without touching the
rest of the emulator.

## Reverse-engineering findings

- OTA is an A/B `payload.bin` (update_engine v2). Full partition set recovered
  into `configs/ota_partitions.json`; boot chain `xbl → abl → boot`, plus AVB
  (`vbmeta*`), `dtbo`, `modem`, and the Android system partitions. **No
  `vendor_boot` and no standalone `dtb` partition** — the DTB is embedded in the
  boot image (header v2).
- Kernel command line and hardware confirmed from the image/DTB
  (`androidboot.hardware=hollywood`, GENI UART console, GICv3, UFS storage,
  SyncBoss sensor MCU over SPI, Adreno 650 via KGSL).

## Boot pipeline status

Phase 1 (prepare machine) and Phase 2 (real ARM64 execution) are complete:

1. Extract `boot` from the OTA, parse the boot image → ARM64 `Image` (29 MB).
2. Parse the embedded DTB → DRAM base `0x80000000`, console UART, GIC.
3. Allocate guest RAM; load kernel `@0x80008000`, DTB `@0x81e08000`, ramdisk
   `@0x82000000`.
4. Set the ARM64 boot state (PC = kernel entry, X0 = DTB) and run Unicorn.

The kernel then **executes ~6634 real instructions** of `head.S`, configures
MAIR/TCR/TTBR0/TTBR1, and enables the MMU.

### Current blocker

Immediately after `msr sctlr_el1, x0` (MMU enable), the first post-MMU
instruction fetch at `0x80fa9278` raises a **prefetch abort (instruction abort)**
under Unicorn/QEMU-TCG — *even though* the emulator's own software page-table
walk proves that address is correctly mapped and executable (identity mapping via
the idmap; access flag set; Normal memory). The backend self-test passes, and the
fault reproduces across CPU model (A57/max), RAM mapping strategy, hook
configuration, single-stepping, and EL1/EL2 entry.

This is a limitation of Unicorn 2.1.4's AArch64 stage-1 instruction-fetch
translation (full `qemu-system-aarch64` boots this kernel). The diagnostics for
this state (register dump, ESR/FAR/VBAR/MAIR, and a software page-table walk of
the faulting PC) are printed on every stop. Next-phase options: a newer/patched
Unicorn build, or swapping the `CpuBackend` to a fuller execution core.
