#include "core/gfx_inject.h"
#include "core/ext4_writer.h"
#include "common/log.h"
#include <cstring>
#include <fstream>
#include <vector>

namespace hw::core {

const char* const kGfxsrcImgPath = R"(D:\gfxbuild\gfxsrc.img)";

// init imports /init.${ro.hardware}.rc from the (post switch_root) system root.
// This mounts the gfx payload partition and launches the SwiftShader test.
const char* const kInitHollywoodRc =
    "# Injected by hollywood_emu (--gfx): SwiftShader + composer3 shim bring-up.\n"
    "on init\n"
    "    # The stock QTI composer opens /dev/dri/card0 (no SDE here) and SIGSEGVs,\n"
    "    # taking SurfaceFlinger down with it; the Oculus one wraps it. Stop both;\n"
    "    # our composer3 shim (adopted from the Quest-Emulator project) replaces them.\n"
    "    stop vendor.qti.hardware.display.composer\n"
    "    stop vendor.oculus.hardware.composer-service\n"
    "\n"
    "on post-fs-data\n"
    "    mkdir /mnt/gfx 0755 root root\n"
    "    mount ext4 /dev/block/by-name/gfxsrc /mnt/gfx\n"
    "    chmod 0755 /mnt/gfx/setup.sh\n"
    "    chmod 0755 /mnt/gfx/questemu-composer-shim\n"
    "    chmod 0755 /mnt/gfx/questemu-strata-shim\n"
    "    start hollywood_logcat\n"
    "    start hollywood_setup\n"
    "    start questemu_composer\n"
    "    start questemu_strata\n"
    "\n"
    // PERSISTENT logcat -> /dev/kmsg. This must be its own long-running service:
    // hollywood_setup is `oneshot`, and when a oneshot exits init kills its whole
    // process group -- so a logcat backgrounded from setup.sh dies at ~16B, long
    // before SurfaceFlinger starts (~25B), and SF's own logs (incl. the reason it
    // aborts) are never captured. Focused tag set with *:S to keep the volume sane.
    "service hollywood_logcat /system/bin/sh -c \"exec logcat -b all -v brief SurfaceFlinger:V SurfaceFlingerVulkan:V HWComposer:V RenderEngine:V QuestEmuComposer:V vulkan:V swiftshader:V SwiftShader:V gralloc4:V Gralloc4:V DEBUG:V libc:V AndroidRuntime:V bootanim:V *:E >/dev/kmsg 2>&1\"\n"
    "    class main\n"
    "    user system\n"
    "    group system log readproc\n"
    "    seclabel u:r:shell:s0\n"
    "    disabled\n"
    "\n"
    "service hollywood_setup /system/bin/sh /mnt/gfx/setup.sh\n"
    "    class core\n"
    "    user root\n"
    "    group root system log graphics\n"
    "    seclabel u:r:shell:s0\n"
    "    oneshot\n"
    "    disabled\n"
    "\n"
    "# The composer3 shim: reports one virtual display so SurfaceFlinger::init\n"
    "# completes, composites layers itself (--composition device, so SF never runs\n"
    "# RenderEngine), and scans out to /mnt/gfx/framebuffer (our capture path).\n"
    // Wrapped in `sh -c "exec ... >/dev/kmsg 2>&1"` so the composer's own stdout/
    // stderr (its "VINTF declares ...: yes/NO" verdict and the addService status
    // code) land in the kernel log directly, instead of depending on logd/logcat
    // being up in time to catch them from the ring buffer.
    // --no-scanout: in Strata mode SF hands the primary display to the Strata shim,
    // which owns /mnt/gfx/framebuffer and scans out the boot animation. The composer
    // still provides the HWC interface SF needs to init, but must NOT also write the
    // framebuffer or it would fight the Strata shim ("two writers, one framebuffer").
    "service questemu_composer /system/bin/sh -c \"exec /mnt/gfx/questemu-composer-shim --width 1024 --height 768 --dpi 320 --no-scanout >/dev/kmsg 2>&1\"\n"
    "    class main\n"
    "    user system\n"
    "    group system graphics drmrpc\n"
    // Run in the REAL composer HAL domain, not shell. servicemanager is a
    // userspace object manager: it reads /sys/fs/selinux/enforce (=1 on this
    // `user` build) and enforces canAdd() itself, so the emulator's kernel-side
    // avc_denied() permissive patch does NOT cover it -- u:r:shell:s0 gets
    // EX_SECURITY (-1) on addService. hal_graphics_composer_default is
    // policy-allowed to add hal_graphics_composer_service; the domain transition
    // + entrypoint from the unlabeled /mnt/gfx binary are kernel avc checks and
    // ARE neutered by that patch, so it still starts.
    "    seclabel u:r:hal_graphics_composer_default:s0\n"
    "    interface aidl android.hardware.graphics.composer3.IComposer/default\n"
    "    interface aidl android.hardware.graphics.composer3.IComposer/display\n"
    "    disabled\n"
    "\n"
    // The Strata shim (adopted from the Quest-Emulator project): Horizon's SF runs
    // in Strata mode (hwc_service_name=display) and hands the PRIMARY display to a
    // separate "Strata" compositor service, which vr_bootanimation blocks on via
    // waitForService("Strata"). Without it nothing composites the primary display,
    // so SF never presents. This publishes an unmarked (SYSTEM stability, no VINTF)
    // binder named "Strata" (labeled strata_service) and scans the boot animation's
    // frames out to /mnt/gfx/framebuffer. Tiny resolution (160x120) because
    // bootanimation CPU-rasterises through SwiftShader under TCG (~30s/frame at
    // 640x480 per the friend's measurements); the shim letterboxes it up.
    "service questemu_strata /system/bin/sh -c \"exec /mnt/gfx/questemu-strata-shim --width 160 --height 120 --frame-log-interval 5 --eye both >/dev/kmsg 2>&1\"\n"
    "    class main\n"
    "    user system\n"
    "    group system graphics drmrpc\n"
    // Must run in the `strata` SELinux domain: system_ext_sepolicy.cil grants only
    // `allow strata strata_service:service_manager add` (bootanim/system_server can
    // only find). servicemanager enforces this add in userspace (kernel-avc permissive
    // patch does NOT cover it), so any other domain gets EX_SECURITY (-1). The
    // transition/entrypoint from the unlabeled /mnt/gfx binary are kernel-avc and ARE
    // neutered, so it still starts.
    "    seclabel u:r:strata:s0\n"
    "    interface aidl Strata\n"
    "    disabled\n";

// composer3 VINTF fragment: injected into /vendor/etc/vintf/manifest so that
// SurfaceFlinger's AidlComposer::isDeclared() is true and it takes the AIDL path
// (the Quest 2 otherwise declares only HIDL composer@2.4). Also lets the shim's
// VINTF-stable binder pass servicemanager's addService check.
const char* const kComposerVintfXml =
    "<manifest version=\"1.0\" type=\"device\">\n"
    "    <hal format=\"aidl\">\n"
    "        <name>android.hardware.graphics.composer3</name>\n"
    "        <version>2</version>\n"
    "        <fqname>IComposer/default</fqname>\n"
    "        <fqname>IComposer/display</fqname>\n"
    "    </hal>\n"
    "</manifest>\n";

bool vbmeta_disable_verity(Bytes& v) {
    if (v.size() < 128) return false;
    if (std::memcmp(v.data(), "AVB0", 4) != 0) return false;
    // flags @ offset 120, uint32 big-endian. Set HASHTREE_DISABLED|VERIFICATION_DISABLED.
    v[120] = 0x00; v[121] = 0x00; v[122] = 0x00; v[123] = 0x03;
    HW_INFO("gfx", "vbmeta: set flags=0x3 (HASHTREE_DISABLED|VERIFICATION_DISABLED) -> dm-verity off");
    return true;
}

bool inject_init_rc(Bytes& system_img, std::string& err) {
    std::string rc = kInitHollywoodRc;
    std::vector<uint8_t> content(rc.begin(), rc.end());
    if (!ext4_add_root_file(system_img, "init.hollywood.rc", content, 0644, err)) {
        err = "ext4 add /init.hollywood.rc: " + err;
        return false;
    }
    HW_INFO("gfx", "injected /init.hollywood.rc ({} bytes) into system root", content.size());
    return true;
}

bool inject_composer_vintf(Bytes& vendor_img, std::string& err) {
    std::string xml = kComposerVintfXml;
    std::vector<uint8_t> content(xml.begin(), xml.end());
    if (!ext4_add_file(vendor_img, "/etc/vintf/manifest/questemu-composer-shim.xml",
                       content, 0644, err)) {
        err = "ext4 add composer vintf: " + err;
        return false;
    }
    HW_INFO("gfx", "injected composer3 VINTF fragment into /vendor/etc/vintf/manifest");
    return true;
}

Bytes load_gfxsrc_img() {
    std::ifstream f(kGfxsrcImgPath, std::ios::binary | std::ios::ate);
    if (!f) { HW_WARN("gfx", "gfxsrc image not found: {}", kGfxsrcImgPath); return {}; }
    size_t n = (size_t)f.tellg(); Bytes b(n); f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), (std::streamsize)n);
    HW_INFO("gfx", "loaded gfxsrc image {} ({} MB)", kGfxsrcImgPath, n / (1024 * 1024));
    return b;
}

} // namespace hw::core
