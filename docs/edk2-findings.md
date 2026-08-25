# EDK2 / Qualcomm ABL findings — boot handoff & timer (for hollywood_emu)

Investigated tree: `edk2-07665c08352d0051a627d5b42ae0ad1487f29afc` (extracted to
`D:\projects\eurka_emu\build\edk2_extract\...`). This is upstream **EDK2 + Qualcomm
`QcomModulePkg`** — i.e. Qualcomm's open-source **ABL / LinuxLoader** boot application
(the `boot_images`/`abl` "LinuxLoader" that runs as a UEFI app and jumps to Linux).
Cross-referenced against the Quest 2 kernel (`oculus-linux-kernel-...-quest2`).

All EDK2 paths below are **relative to the edk2 tree root**.
Kernel paths are relative to the kernel tree root.

---

## What this tree IS and IS NOT (read this first)
- It contains the standard EDK2 packages (`ArmPkg`, `MdePkg`, `MdeModulePkg`, ...) **plus**
  one Qualcomm package: `QcomModulePkg` (LinuxLoader app + BootLib). Root has
  `AndroidBoot.mk`, `BUILD.bazel`, `.gn` → this is the Android/Qualcomm ABL fork of EDK2.
- It does **NOT** contain the closed Qualcomm silicon package (`QcomPkg`, the SEC/PEI/DXE
  platform init). So the **low-level CPU/EL bring-up, GIC DXE init, and CNTFRQ programming
  are NOT in this tree** — they live in the closed XBL / QHEE / QcomPkg. This tree only
  covers the *final UEFI-app→Linux handoff*, which is exactly the boot-handoff topic.
- `QcomModulePkg` source is **SoC-agnostic**: it queries chip/board info at runtime. SoC
  targeting is expressed only through per-SoC **build configs**, of which Kona is one of ~50.

---

## CONFIRMED (Kona / SM8250 / XR2-specific)
- **This tree explicitly supports Kona (SM8250 = Snapdragon 865 / XR2).**
  - `QcomModulePkg/build.config.msm.kona` → `MSM_ARCH=kona`, `BOOTLOADER_ARCH=AARCH64`.
  - Also `QcomModulePkg/build.config.msm.kona.le`.
  - (Kona is the SoC codename for SM8250; the XR2 in Quest 2 is the SM8250-derived part.)
- **Kernel DT confirms the target and every hardware address the emulator uses**
  (`arch/arm64/boot/dts/vendor/qcom/kona.dtsi`):
  - `compatible = "qcom,kona"`; CPUs `compatible = "qcom,kryo"`, `enable-method = "psci"`.
  - **GICv3** `interrupt-controller@17a00000 { compatible = "arm,gic-v3"; ...
    reg = <0x17a00000 0x10000> /*GICD*/, <0x17a60000 0x100000> /*GICR x8*/;
    #redistributor-regions = <1>; redistributor-stride = <0x0 0x20000>;
    interrupts = <GIC_PPI 9 ...>; }` → **GICD 0x17a00000, GICR 0x17a60000 — MATCHES.**
  - **Arch timer** `compatible = "arm,armv8-timer"; clock-frequency = <19200000>;`
    interrupts (in DT order): `<PPI 13>` secure-phys, `<PPI 14>` non-secure-phys(EL1),
    `<PPI 11>` virtual, `<PPI 12>` hyp(EL2). → **19.2 MHz MATCHES.**
  - **Mem-mapped timer** `timer@17c20000 { compatible = "arm,armv7-timer-mem";
    reg = <0x17c20000 0x1000>; clock-frequency = <19200000>; }` → **MATCHES.**
  - **PSCI** `psci { compatible = "arm,psci-1.0"; method = "smc"; }` →
    **PSCI 1.0, SMC conduit — MATCHES.** (`qcom,kona-pdc` @ 0xb220000 is the PDC wakeup irqchip.)

## LIKELY Qualcomm / Kona (mechanism from ABL; applies to the Kona build)
- **64-bit kernel entry = plain function-pointer jump at the loader's CURRENT EL — no EL
  change.** `QcomModulePkg/Library/BootLib/BootLinux.c:1981-1982`:
  ```c
  LinuxKernel = (LINUX_KERNEL)(UINT64)BootParamlistPtr.KernelLoadAddr;
  LinuxKernel((UINT64)BootParamlistPtr.DeviceTreeLoadAddr, 0, 0, 0);   // x0 = DTB, x1..x3 = 0
  ```
  ABL does not drop/raise EL for a 64-bit kernel; Linux inherits whatever EL the ABL runs at.
- **32-bit path is the only one that changes EL, and it targets EL1.**
  `BootLinux.c:350-368` `SwitchTo32bitModeBooting()` fills `EFI_HLOS_BOOT_ARGS`
  (`el1_elr = KernelLoadAddr`, `el1_x2 = DTB`) and calls
  `pQcomScmModeSwitchProtocol->SwitchTo32bitMode()` — an **SCM (EL3 monitor) call** that
  returns into **EL1** (`EFIScmModeSwitch.h`). The `el1_*` field naming is direct evidence
  that the HLOS/Linux world on this platform runs at **NS-EL1**, with EL3 = secure monitor.
- **Pre-jump CPU state (standard arm64 boot protocol).**
  `QcomModulePkg/Library/BootLib/ShutdownServices.c:137-177` `PreparePlatformHardware()`:
  disables branch prediction, **masks interrupts**, disables async abort, cleans+invalidates
  D-cache, invalidates I-cache, **disables D-cache, I-cache, MMU**, invalidates TLB.
  Runs right after `ShutdownUefiBootServices()` (`BootLinux.c:1939,1950`). So Linux is
  entered with **MMU off, caches off, IRQs masked** — but at the same EL as ABL.
- **Hypervisor (EL2) present in the chain.** ABL includes
  `QcomModulePkg/Library/BootLib/HypervisorMvCalls.c/.h`, merges a "hyp dtbo"
  (`BootLinux.c:697-708, 793-804`), and uses `HLOS_VMID 3`. Combined with the `el1_*`
  handoff, this indicates a Qualcomm hypervisor (QHEE/Gunyah) occupies **EL2** and Linux
  runs at **EL1** on production hardware.

## GENERIC Qualcomm / EDK2 (may or may not match the closed Kona init)
- **Firmware does NOT program `CNTFRQ_EL0` on AArch64 — it delegates to secure firmware.**
  `ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.c:52-68`: the `CNTFRQ` write is guarded by
  `#ifdef MDE_CPU_ARM` (ARM32 only). Comments: *"Only set the frequency for ARMv7. We expect
  the secure firmware to have already done it"* and *"Architectural Timer Frequency must be
  set in Secure privileged mode."* On AArch64 the constructor only ASSERTs `CNTFRQ != 0`.
- **No timer-frequency PCD override.** `gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz` default
  `= 0` (`ArmPkg/ArmPkg.dec:222`); `QcomModulePkg/QcomModulePkg.dsc` does **not** set it
  (TimerLib = `ArmPkg/.../ArmArchTimerLib.inf`). So EDK2 reads the rate from HW and never
  writes it → **CNTFRQ_EL0 = 19200000 must be established by EL3/XBL before UEFI, not by ABL.**
- **No `CNTVOFF_EL2` / `CNTHCTL_EL2` programming anywhere in `QcomModulePkg`.** Those are the
  EL2/hypervisor's responsibility (QHEE), not in this tree.
- **No GIC-for-Linux init in ABL.** ABL does not touch GICD/GICR before the jump; GIC DXE
  init lives in the closed QcomPkg, and the kernel re-initialises the GICv3 itself from DT.

## IRRELEVANT / different SoC
- ~50 other `build.config.msm.*` targets exist (kalama, lahaina is absent by that name but
  "kona" and many newer parts present: pineapple/sun/anorak/etc.). Only the **kona** configs
  and `kona.dtsi` are relevant to Quest 2.
- Huge `QcomModulePkg/Library/OpenDice/boringssl/...` and AVB/Fastboot trees — verified-boot
  crypto, not relevant to the timer/EL question.

---

## Relevance to the arch_timer blocker ("arch_timer: No interrupt available, giving up")

Source of the message: `drivers/clocksource/arm_arch_timer.c` (Quest 2 kernel).

**Two independent facts settle what the message means:**

1. **It is NOT a frequency problem.** `arch_timer_of_configure_rate()`
   (`arm_arch_timer.c:961-973`) reads `clock-frequency` from the DT **first** and only falls
   back to `CNTFRQ_EL0` if the DT property is missing. kona.dtsi provides
   `clock-frequency = <19200000>`, so an unset `CNTFRQ_EL0` does **not** trigger this error.
   (You should still set `CNTFRQ_EL0 = 19200000` for correct timekeeping / VDSO, but it is
   not the blocker.)

2. **The message is an IRQ-mapping failure of the *selected* timer PPI.**
   `arch_timer_of_init()` (`arm_arch_timer.c:1322-1359`):
   - maps all 4 PPIs: `arch_timer_ppi[i] = irq_of_parse_and_map(np, i)` for i=0..3;
   - picks one via `arch_timer_select_ppi()`;
   - if `arch_timer_ppi[selected] == 0` → **"No interrupt available, giving up"**.
   Since kona.dtsi lists all 4 PPIs, a zero here means the **GICv3 irq domain failed to
   translate/map the PPI** — i.e. an emulated-GIC problem (redistributor / PPI domain not
   correctly presented per-CPU), **not** a timer problem.

**The entry-EL decides WHICH PPI must map** (`arch_timer_select_ppi()`, `arm_arch_timer.c:1308-1320`):
| Entry condition | `is_hyp_mode_*` | PPI chosen | DT index | GIC_PPI (local) | GIC INTID |
|---|---|---|---|---|---|
| **EL1** (current emu workaround) | hyp NOT available | `ARCH_TIMER_VIRT_PPI` (virtual timer) | 2 | 11 | **27** |
| **EL2, no VHE** | hyp available, not in it | `ARCH_TIMER_PHYS_NONSECURE_PPI` | 1 | 14 | **30** |
| **EL2 + VHE** (kernel in hyp) | `is_kernel_in_hyp_mode()` | `ARCH_TIMER_HYP_PPI` | 3 | 12 | **26** |

(`arm,cpu-registers-not-fw-configured` → SECURE PPI is ARM32-only, guarded by
`IS_ENABLED(CONFIG_ARM)` at line 1350; irrelevant on arm64. kona.dtsi does not set it.)

**Verdict on the EL2-entry hypothesis:**
- The handoff evidence (64-bit = jump at current EL; 32-bit switch returns to **el1_elr**;
  `HypervisorMvCalls` + hyp-dtbo + `HLOS_VMID`) indicates real Quest 2 boots **Linux at NS-EL1
  under a Qualcomm hypervisor at EL2**. Under that model the kernel selects the **virtual
  timer (PPI 11 / INTID 27)** — which is exactly the PPI an **EL1** entry selects. So the
  emulator's EL1 entry is *architecturally plausible/correct*, and the blocker is the GICv3
  not mapping the virtual-timer PPI, **not** the exception level.
- Therefore the primary fix is in the **GICv3 emulation**: ensure `irq_of_parse_and_map` for
  the arch-timer node succeeds — i.e. the redistributor at 0x17a60000 (stride 0x20000, one
  region) is correctly presented and PPIs are routable per-CPU, so **PPI 11 → INTID 27**
  resolves for EL1 entry.
- If you instead switch the emulator to **EL2 entry** to match "real HW" more literally, the
  kernel will pick a **different** PPI (14/INTID 30 without VHE, or 12/INTID 26 with VHE), and
  you must (a) wire that PPI in the GIC and (b) also set `CNTVOFF_EL2 = 0` and
  `CNTHCTL_EL2` (EL1PCTEN/EL1PCEN) so EL1 timer access is not trapped. Entering at EL2 does
  **not** by itself fix a broken PPI irq-domain — it only changes which PPI must work.

**Concrete register/entry conditions to replicate (given EL1 entry):**
- Enter kernel: MMU off, D/I-cache off, IRQs masked, x0 = DTB phys addr, x1=x2=x3=0.
- `CNTFRQ_EL0 = 19200000` (0x124F800) — set by "EL3/XBL", i.e. before the kernel; not
  strictly required for this error but required for correct time.
- Virtual timer usable at EL1: if EL2 is *not* implemented in the model, CNTVOFF is moot
  (virt==phys count); if EL2 *is* implemented, provide `CNTVOFF_EL2 = 0` and
  `CNTHCTL_EL2` with EL1PCTEN|EL1PCEN set.
- GICv3: GICD 0x17a00000, GICR 0x17a60000 (stride 0x20000); make the arch-timer PPIs
  routable so INTID 27 (virtual, PPI 11) maps for EL1 entry.
- PSCI 1.0 over **SMC** must be answered by the model's EL3/monitor stub (CPUs use
  `enable-method="psci"`).

---

### Key source references
- `QcomModulePkg/build.config.msm.kona` — `MSM_ARCH=kona`, AARCH64.
- `QcomModulePkg/Library/BootLib/BootLinux.c:1936-1990` — ShutdownUefiBootServices →
  PreparePlatformHardware → jump; `:350-368` 32-bit SCM EL-switch to el1_elr.
- `QcomModulePkg/Library/BootLib/ShutdownServices.c:137-177` — PreparePlatformHardware
  (MMU/cache/IRQ teardown).
- `QcomModulePkg/Include/Protocol/EFIScmModeSwitch.h` — EFI_HLOS_BOOT_ARGS (el1_elr/el1_spsr).
- `ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.c:44-70` — CNTFRQ set is ARM32-only;
  AArch64 relies on secure firmware.
- `ArmPkg/ArmPkg.dec:222` — PcdArmArchTimerFreqInHz default 0.
- kernel `arch/arm64/boot/dts/vendor/qcom/kona.dtsi` — gic-v3 @17a00000/@17a60000,
  armv8-timer 19.2MHz (PPIs 13/14/11/12), armv7-timer-mem @17c20000, psci-1.0 smc.
- kernel `drivers/clocksource/arm_arch_timer.c:961-973` (rate from DT), `:1308-1320`
  (PPI select by EL), `:1322-1359` (map + "No interrupt available, giving up").
