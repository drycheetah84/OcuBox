#include "cpu/unicorn_cpu.h"
#include "cpu/keymaster_ta.h"
#include <vector>
#include "common/log.h"
#include "devices/device.h"
#include "devices/device_bus.h"
#include "devices/irq.h"
#include "memory/guest_memory.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <unicorn/unicorn.h>
#include <capstone/capstone.h>

// ---- NON-FAITHFUL keymaster/QSEE SCM shim (user-enabled via --kmshim) ----
// The real Qualcomm TrustZone/QSEE cannot complete cold boot in the emulator
// (needs device fuses + undocumented xbl secure-world data), so with --kmshim the
// kernel's non-PSCI SMC calls (Qualcomm SiP / qseecom / keymaster) are dispatched
// here (from psci.c's arm_handle_psci_call). This EMULATES the secure world -- the
// boot is explicitly non-faithful past this point.
extern "C" { extern bool (*hollywood_scm_handler_fn)(void*); }
namespace {
// ============================ QSEE COMPATIBILITY LAYER ============================
// NOT faithful QSEE. Synthesizes the MINIMUM QSEE_OS (owner=50) / TZ_APPS (owner=48)
// SMC responses the REAL qseecom kernel driver + keymaster/gatekeeper HALs need to
// get past QSEE application discovery/startup, per the ABI established in Phase 15
// (kernel include/soc/qcom/qseecomi.h + drivers/misc/qseecom.c).
//
//   FAITHFUL      : SMC encode/decode, the qcom_scm/qseecom ABI, the real userspace
//                   callers (qseecomd, keymaster HAL), the real kernel qseecom driver,
//                   the real signed TA binaries in the OTA.
//   COMPATIBILITY : QSEE app discovery (APP_LOOKUP), app startup (APP_START) with
//                   deterministic synthetic app_ids, listener registration.
//   SYNTHETIC     : the device root-of-trust -- fuse-derived secrets, RPMB auth key,
//                   attestation keys, hardware-backed key material. NOT implemented
//                   here. A TA command (owner=48 SEND_DATA) needs the real TA + these
//                   secrets, so it is LOGGED as the next real boundary, NOT faked.
//
// Response ABI (Phase 15): the SMC returns X0=0 (transport OK), then X1=result
// (QSEOS status), X2=resp_type, X3=data (app_id/listener_id); the driver reads
// {result,resp_type,data} = {ret0,ret1,ret2} = {X1,X2,X3}.
//
// STRATEGY (Phase 21): this is an ANDROID-COMPATIBILITY target, NOT a genuine Quest.
// We do NOT load real TAs or reproduce XBL / the device root-of-trust. APP_LOOKUP returns
// "already loaded" (as if XBL had preloaded the TA): the driver's QUERY handler then
// REGISTERS the app (qseecom.c:6060, the "loaded by appsbl before" path -> registered_app_list),
// so the HAL skips the (absent) TA image file AND later SEND_DATA can find the app. SEND_DATA
// then carries the keymaster/gatekeeper TA command protocol, which we answer with the minimum
// deterministic synthetic response Android needs to proceed (implemented per observed command,
// not blind SUCCESS). Synthetic key material is emulator-backed + consistent, never genuine.
constexpr uint64_t QSEE_RESULT_SUCCESS = 0;
constexpr uint64_t QSEE_RESULT_FAILURE = 0xFFFFFFFFull;   // QSEOS_RESULT_FAILURE
constexpr uint64_t QSEE_TYPE_APP_ID    = 0xEE01;          // QSEOS_APP_ID
constexpr uint32_t TZ_OWNER_TZ_APPS    = 48;
constexpr uint32_t TZ_OWNER_QSEE_OS    = 50;

struct QseeCompatState {
    std::unordered_map<std::string, uint32_t> loaded;    // TA name -> synthetic app_id
    std::unordered_map<uint32_t, std::string> app_names; // app_id -> TA name (for SEND_DATA)
    std::unordered_set<uint32_t> listeners;              // registered listener ids
    uint32_t next_app_id = 0x1001;                       // deterministic; never a host ptr
    std::string pending_load;                            // name from the last APP_LOOKUP
    int log = 0;                                          // capped [QSEE-COMPAT] budget
};
QseeCompatState g_qsee;

std::string qsee_read_name(uc_engine* uc, uint64_t pa, uint64_t len) {
    char buf[36] = {0};                                // MAX_APP_NAME_SIZE = 32
    size_t n = (size_t)(len < sizeof(buf) - 1 ? len : sizeof(buf) - 1);
    if (n && uc_mem_read(uc, pa, buf, n) != UC_ERR_OK) n = 0;
    buf[n] = 0;
    return std::string(buf);
}

// ---- QSEE trusted-app SEND_DATA command dispatch (COMPATIBILITY / SYNTHETIC) ----
// This decodes the real TA command protocol (the wire structs match the OTA/ABL
// KeymasterClient.c) and writes the minimum deterministic response Android needs to
// proceed. We do NOT reproduce the secure world; key material is emulator-backed and
// consistent, never genuine/attestable. Each command is implemented as it is observed
// (not blind SUCCESS): an unrecognized command logs its bytes and returns false so the
// driver reports a real error and the next blocker is visible.
//
// keymaster TA command ids (KeymasterClient.c): KEYMASTER_CMD_ID=0x100 base for the
// HIDL/keymint ops (generate/import/begin/update/finish/...), KEYMASTER_UTILS_CMD_ID
// =0x200 base for utils (GET_VERSION=0x200, SET_ROT=0x201, ...).
constexpr uint32_t KM_CMD_BASE   = 0x100;
constexpr uint32_t KM_UTILS_BASE = 0x200;
constexpr uint32_t KM_GET_VERSION    = KM_UTILS_BASE + 0;    // 0x200
constexpr uint32_t KM_SET_ROT        = KM_UTILS_BASE + 1;    // 0x201  KMSetRotRsp{Status}
constexpr uint32_t KM_SET_VERSION    = KM_UTILS_BASE + 7;    // 0x207  ack {Status}
constexpr uint32_t KM_SET_BOOT_STATE = KM_UTILS_BASE + 8;    // 0x208  KMSetBootStateRsp{Status}
constexpr uint32_t KM_SET_VBH        = KM_UTILS_BASE + 17;   // 0x211  KMSetVbhRsp{Status}
constexpr uint32_t KM_GET_DATE_SUPPORT = KM_UTILS_BASE + 21; // 0x215  KMGetDateSupportRsp{Status}
constexpr uint32_t KM_CONFIGURE      = KM_CMD_BASE + 22;     // 0x116  keymaster configure()

// Observation aid: with KM_PROBE set in the environment, an otherwise-unhandled TA
// command returns a zeroed {status=0} response instead of FAILURE, so the HAL keeps
// issuing its init sequence and the WHOLE command list is visible in one boot (the HAL
// aborts on the first real FAILURE otherwise). This is a TRACING tool, not a solution:
// the zeroed responses are not valid key material, so it never actually mounts /data --
// each command it reveals must then be implemented properly. Off by default.
bool g_km_probe = false;
bool g_km_probe_init = false;

// Write `n` bytes to guest phys rsp_ptr iff the buffer is present + large enough.
bool qsee_put_rsp(uc_engine* uc, uint64_t rsp_ptr, uint64_t rsp_len,
                  const void* p, size_t n) {
    if (!rsp_ptr || rsp_len < n) return false;
    return uc_mem_write(uc, rsp_ptr, p, n) == UC_ERR_OK;
}

// Returns true (SUCCESS) if a response was produced, false (FAILURE) to expose the
// next real blocker. `lg` gates the capped compat log.
bool qsee_ta_dispatch(uc_engine* uc, const std::string& name, uint32_t cmd,
                      uint64_t req_ptr, uint64_t req_len,
                      uint64_t rsp_ptr, uint64_t rsp_len, bool lg) {
    (void)req_ptr; (void)req_len;
    if (name == "keymaster64" || name == "keymaster") {
        switch (cmd) {
        case KM_GET_VERSION: {
            // KMGetVersionRsp { INT32 Status; UINT32 Major, Minor, AppMajor, AppMinor; }
            // Client requires Status==0 && Major>=2 (KeymasterClient.c:223/313). All
            // fields are 4-byte, so the layout is a packed 20 bytes with no padding.
            struct KMGetVersionRsp {
                int32_t status; uint32_t major, minor, app_major, app_minor;
            } r = { 0, 3, 4, 4, 1 };
            bool ok = qsee_put_rsp(uc, rsp_ptr, rsp_len, &r, sizeof r);
            if (lg) std::printf("[QSEE-COMPAT]   KM GET_VERSION -> {st=0,maj=3,min=4,app=4.1} put=%d\n", ok);
            return ok;
        }
        case KM_SET_ROT:
        case KM_SET_VERSION:
        case KM_SET_BOOT_STATE:
        case KM_SET_VBH: {
            // These "utils set" commands acknowledge with { INT32 Status } (the wire
            // structs KMSetRotRsp/KMSetBootStateRsp/KMSetVbhRsp are all just {Status}).
            // Status=0 (KM_ERROR_OK). These configure the TA's view of ROT/version/boot
            // state -- for a compatibility target we accept them; there is no genuine ROT.
            int32_t status = 0;
            bool ok = qsee_put_rsp(uc, rsp_ptr, rsp_len, &status, sizeof status);
            if (lg) std::printf("[QSEE-COMPAT]   KM cmd %#x (ack) -> {status=0} put=%d\n", cmd, ok);
            return ok;
        }
        case KM_GET_DATE_SUPPORT: {
            // KMGetDateSupportRsp is {INT32 Status} in the ABL; the HAL may also read a
            // "supported" flag right after, so zero 8 bytes -> {status=0, supported=0}.
            // "not supported" is the safe compat answer (no date-based-validity feature).
            uint32_t r[2] = { 0, 0 };
            bool ok = qsee_put_rsp(uc, rsp_ptr, rsp_len, r, sizeof r);
            if (lg) std::printf("[QSEE-COMPAT]   KM GET_DATE_SUPPORT -> {status=0,supported=0} put=%d\n", ok);
            return ok;
        }
        default: break;
        }
    }
    // Unhandled. In KM_PROBE tracing mode, return a zeroed success so the HAL keeps
    // issuing commands (reveals the full sequence in one boot); otherwise FAILURE so the
    // next real blocker is exposed. Probe responses are NOT valid -- for tracing only.
    if (g_km_probe && rsp_ptr && rsp_len) {
        uint8_t z[128] = {0};
        size_t n = (size_t)(rsp_len < sizeof z ? rsp_len : sizeof z);
        bool ok = uc_mem_write(uc, rsp_ptr, z, n) == UC_ERR_OK;
        if (lg) std::printf("[QSEE-COMPAT]   PROVISIONAL(probe) app=%s cmd=%#x -> zeroed %zuB put=%d\n",
                            name.c_str(), cmd, n, ok);
        return ok;
    }
    if (lg) std::printf("[QSEE-COMPAT]   UNHANDLED TA cmd app=%s cmd=%#x -> FAILURE (observe)\n",
                        name.c_str(), cmd);
    return false;
}

// ---- SELinux permissive (compatibility) ----
// The Quest is a `user` build, so init forces SELinux enforcing regardless of
// androidboot.selinux=permissive. We neuter the kernel denial path directly:
// avc_denied() (security/selinux/avc.c) returns -EACCES when enforcing and 0 when
// permissive, so a single-address code hook that forces X0=0 + return makes every
// would-be denial permissive. This removes SELinux (incl. silent dontaudit'd binder
// denials) as a variable for the compat boot and lets crash_dump ptrace failing
// daemons. Address from build/ksyms.txt for this Quest kernel; avc_denied is regular
// .text so the hook matches on its kernel VA (no PA/struct-offset math needed).
constexpr uint64_t kAvcDeniedVA = 0xffffff80084866e0ull;
void selinux_permissive_cb(uc_engine* uc, uint64_t /*address*/, uint32_t /*size*/, void* /*user*/) {
    uint64_t zero = 0, lr = 0;
    uc_reg_read(uc, UC_ARM64_REG_X30, &lr);          // return address (LR)
    uc_reg_write(uc, UC_ARM64_REG_X0, &zero);        // avc_denied() -> 0 (allow)
    uc_reg_write(uc, UC_ARM64_REG_PC, &lr);          // skip the body, return to caller
}

// ---- guest reboot detection ----
// machine_restart() issues a PSCI reset then spins forever (b .) waiting for a reset the
// emulator never performs -- wasting billions of instructions. A hook on its entry flags a
// reboot and stops the run loop cleanly, which triggers the halt-time kernel-log dump so the
// reboot reason is visible immediately. Address from build/ksyms.txt for this Quest kernel.
constexpr uint64_t kMachineRestartVA = 0xffffff80080894acull;
volatile bool g_guest_reboot = false;
void machine_restart_cb(uc_engine* uc, uint64_t /*address*/, uint32_t /*size*/, void* /*user*/) {
    if (!g_guest_reboot)
        std::printf("[emu] guest reached machine_restart -> REBOOT requested; halting to dump kernel log.\n");
    std::fflush(stdout);
    g_guest_reboot = true;
    uc_emu_stop(uc);
}

// Handle a QSEE_OS (owner=50) or TZ_APPS (owner=48) SMC. Sets r1/r2/r3 (r0 stays 0).
void qsee_compat(uc_engine* uc, uint32_t owner, uint32_t svc, uint32_t func,
                 const uint64_t x[8], uint64_t& r1, uint64_t& r2, uint64_t& r3) {
    const bool lg = (g_qsee.log < 2000);
    if (lg) g_qsee.log++;

    if (owner == TZ_OWNER_TZ_APPS) {
        // TA SEND_DATA: app_id=args[0]=x2, req_ptr=args[1]=x3, req_len=args[2]=x4.
        // rsp_ptr=args[3], rsp_len=args[4] spill to the SCM overflow buffer at x5
        // (scm.c: arglen>N_REGISTER_ARGS=4 -> x5=phys(extra_arg_buf), args64[0]=args[3]).
        uint32_t app_id = (uint32_t)x[2];
        uint64_t req_ptr = x[3], req_len = x[4];
        auto ni = g_qsee.app_names.find(app_id);
        std::string nm = (ni != g_qsee.app_names.end()) ? ni->second : std::string("?");

        uint32_t cmd_id = 0;
        if (req_ptr) uc_mem_read(uc, req_ptr, &cmd_id, 4);
        uint64_t rsp_ptr = 0, rsp_len = 0;
        if (x[5]) {
            uc_mem_read(uc, x[5], &rsp_ptr, 8);
            uc_mem_read(uc, x[5] + 8, &rsp_len, 8);
        }
        if (lg) {
            std::printf("[QSEE-COMPAT] SEND_DATA app=%s(id=%#x) cmd=%#x req=%#llx/%llu rsp=%#llx/%llu\n",
                        nm.c_str(), app_id, cmd_id, (unsigned long long)req_ptr,
                        (unsigned long long)req_len, (unsigned long long)rsp_ptr,
                        (unsigned long long)rsp_len);
            uint8_t b[64] = {0};
            if (req_ptr && uc_mem_read(uc, req_ptr, b, sizeof b) == UC_ERR_OK) {
                std::printf("[QSEE-COMPAT]   req[0..63]:");
                for (int j = 0; j < 64; j++) std::printf(" %02x", b[j]);
                std::printf("\n");
            }
            std::fflush(stdout);
        }
        if (!g_km_probe_init) { g_km_probe = std::getenv("KM_PROBE") != nullptr; g_km_probe_init = true;
            if (g_km_probe) std::printf("[QSEE-COMPAT] KM_PROBE mode ON (provisional zeroed responses for tracing)\n"); }

        // keymaster HIDL/CBOR path: commands carry the 0x2000 flag and a CBOR payload
        // after the 4-byte command id. Route to the synthetic keymaster TA. The raw
        // ABL struct commands (GET_VERSION/SET_VERSION/... , no 0x2000) fall through.
        if ((nm == "keymaster64" || nm == "keymaster") && (cmd_id & 0x2000)) {
            std::vector<uint8_t> req, krsp;
            if (req_ptr && req_len > 4) {
                req.resize((size_t)req_len - 4);
                uc_mem_read(uc, req_ptr + 4, req.data(), req.size());
            }
            bool kok = km::keymaster_ta_handle(cmd_id, req.data(), req.size(), krsp, lg);
            if (kok && !krsp.empty() && rsp_ptr && krsp.size() <= rsp_len)
                uc_mem_write(uc, rsp_ptr, krsp.data(), krsp.size());
            r1 = kok ? QSEE_RESULT_SUCCESS : QSEE_RESULT_FAILURE; r2 = 0; r3 = 0;
            if (lg) std::fflush(stdout);
            return;
        }

        bool ok = qsee_ta_dispatch(uc, nm, cmd_id, req_ptr, req_len, rsp_ptr, rsp_len, lg);
        r1 = ok ? QSEE_RESULT_SUCCESS : QSEE_RESULT_FAILURE;   // 0 -> driver returns rsp to HAL
        r2 = 0; r3 = 0;
        if (lg) std::fflush(stdout);
        return;
    }

    // owner == TZ_OWNER_QSEE_OS
    if (svc == 1 && func == 3) {                    // TZ_OS_APP_LOOKUP
        // Compat: always report the TA as ALREADY LOADED with a deterministic synthetic
        // app_id (as if XBL preloaded it). The driver's QUERY handler registers it
        // (qseecom.c:6060), so the HAL never needs the (absent) TA image file.
        std::string name = qsee_read_name(uc, x[2], x[3]);
        auto it = g_qsee.loaded.find(name);
        uint32_t aid;
        if (it != g_qsee.loaded.end()) aid = it->second;
        else { aid = g_qsee.next_app_id++; g_qsee.loaded[name] = aid; g_qsee.app_names[aid] = name; }
        g_qsee.pending_load = name;
        r1 = QSEE_RESULT_SUCCESS; r2 = QSEE_TYPE_APP_ID; r3 = aid;
        if (lg) std::printf("[QSEE-COMPAT] APP_LOOKUP name=%s -> LOADED (compat/preloaded) app_id=%#x\n",
                            name.c_str(), aid);
    } else if (svc == 1 && func == 1) {             // TZ_OS_APP_START (driver mapped the .mbn)
        std::string name = g_qsee.pending_load.empty() ? std::string("app") : g_qsee.pending_load;
        auto it = g_qsee.loaded.find(name);
        uint32_t aid = (it != g_qsee.loaded.end()) ? it->second : g_qsee.next_app_id++;
        g_qsee.loaded[name] = aid;
        g_qsee.pending_load.clear();
        r1 = QSEE_RESULT_SUCCESS; r2 = QSEE_TYPE_APP_ID; r3 = aid;
        if (lg) std::printf("[QSEE-COMPAT] APP_START name=%s mdt_len=%#llx img_len=%#llx -> app_id=%#x\n",
                            name.c_str(), (unsigned long long)x[2], (unsigned long long)x[3], aid);
    } else if (svc == 2 && (func == 1 || func == 6)) {   // TZ_OS_REGISTER_LISTENER (+smcinvoke)
        uint32_t lid = (uint32_t)x[2];
        g_qsee.listeners.insert(lid);
        r1 = QSEE_RESULT_SUCCESS; r2 = 0; r3 = 0;
        if (lg) std::printf("[QSEE-COMPAT] REGISTER_LISTENER id=%#x -> SUCCESS\n", lid);
    } else if (svc == 2 && func == 2) {             // TZ_OS_DEREGISTER_LISTENER
        g_qsee.listeners.erase((uint32_t)x[2]);
        r1 = QSEE_RESULT_SUCCESS; r2 = 0; r3 = 0;
        if (lg) std::printf("[QSEE-COMPAT] DEREGISTER_LISTENER id=%#x -> SUCCESS\n", (uint32_t)x[2]);
    } else if (svc == 1 && func == 7) {             // TZ_OS_LOAD_SERVICES_IMAGE (cmnlib)
        r1 = QSEE_RESULT_SUCCESS; r2 = 0; r3 = 0;
        if (lg) std::printf("[QSEE-COMPAT] LOAD_SERVICES_IMAGE (cmnlib) -> SUCCESS\n");
    } else if (svc == 3 && func == 1) {             // TZ_OS_LOAD_EXTERNAL_IMAGE
        r1 = QSEE_RESULT_SUCCESS; r2 = 0; r3 = 0;
        if (lg) std::printf("[QSEE-COMPAT] LOAD_EXTERNAL_IMAGE -> SUCCESS\n");
    } else if (svc == 1 && (func == 4 || func == 5 || func == 6)) {  // GET_STATE/REGION_NOTIF/REG_LOG
        r1 = QSEE_RESULT_SUCCESS; r2 = 0; r3 = 0;
        if (lg) std::printf("[QSEE-COMPAT] APP_MGR func=%u -> SUCCESS\n", func);
    } else {                                        // any other QSEE_OS cmd: don't fake success
        r1 = QSEE_RESULT_FAILURE; r2 = 0; r3 = 0;
        if (lg) std::printf("[QSEE-COMPAT] UNSUPPORTED owner=50 svc=%u func=%u -> FAILURE\n", svc, func);
    }
}

int g_scm_log = 0;       // SIP (owner=2) info/PIL/memprot calls
int g_scm_noisy = 0;     // early-kernel mem-protect/SMMU storm (svc 5/10/12/25)
bool scm_handler_impl(void* ucv) {
    uc_engine* uc = static_cast<uc_engine*>(ucv);
    uint64_t x[8] = {0};
    for (int i = 0; i < 8; i++) uc_reg_read(uc, UC_ARM64_REG_X0 + i, &x[i]);
    uint64_t fn = x[0];
    uint32_t owner = (uint32_t)((fn >> 24) & 0x3f);
    uint32_t svc   = (uint32_t)((fn >> 8) & 0xff);
    uint32_t cmd   = (uint32_t)(fn & 0xff);
    uint64_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;   // r0 = SMCCC/transport status (0 = OK)

    // ---- QSEE COMPATIBILITY LAYER: owner=50 (QSEE_OS) + owner=48 (TZ_APPS) ----
    if (owner == TZ_OWNER_QSEE_OS || owner == TZ_OWNER_TZ_APPS) {
        qsee_compat(uc, owner, svc, cmd, x, r1, r2, r3);
        uc_reg_write(uc, UC_ARM64_REG_X0, &r0);
        uc_reg_write(uc, UC_ARM64_REG_X1, &r1);
        uc_reg_write(uc, UC_ARM64_REG_X2, &r2);
        uc_reg_write(uc, UC_ARM64_REG_X3, &r3);
        return true;
    }

    // ---- SIP services (owner=2): faithful decode of the kernel/qseecom probe path ----
    if (svc == 6 && cmd == 1) {          // IS_CALL_AVAIL: report the queried call present
        r1 = 1;
    } else if (svc == 6 && cmd == 3) {   // GET_FEATURE_VERSION: report a non-zero QSEE version
        r1 = 0x00800000;                 // ~QSEE 4.0 baseline
    }
    bool noisy = (svc == 5 || svc == 10 || svc == 12 || svc == 25);
    int& counter = noisy ? g_scm_noisy : g_scm_log;
    int cap = noisy ? 60 : 200000;
    if (counter < cap) { counter++;
        std::printf("[scm-shim] fn=%#llx (own=%u svc=%u cmd=%u) a1=%#llx a2=%#llx a3=%#llx a4=%#llx -> r1=%#llx\n",
                    (unsigned long long)fn, owner, svc, cmd,
                    (unsigned long long)x[1], (unsigned long long)x[2],
                    (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)r1);
        std::fflush(stdout);
    }
    uc_reg_write(uc, UC_ARM64_REG_X0, &r0);
    uc_reg_write(uc, UC_ARM64_REG_X1, &r1);
    uc_reg_write(uc, UC_ARM64_REG_X2, &r2);
    uc_reg_write(uc, UC_ARM64_REG_X3, &r3);
    return true;
}
} // namespace
void hollywood_install_scm_shim() { hollywood_scm_handler_fn = scm_handler_impl; }

namespace hw::cpu {

std::atomic<uint64_t> g_live_insns{0};
std::atomic<uint64_t> g_live_pc{0};

namespace {
// x0..x30 register ids. X0..X28 are contiguous in Unicorn's enum; x29/x30
// are the frame pointer / link register.
int xreg(int i) {
    if (i <= 28) return UC_ARM64_REG_X0 + i;
    return (i == 29) ? UC_ARM64_REG_X29 : UC_ARM64_REG_X30;
}
constexpr uint32_t kHotBits = 16;
constexpr uint32_t kHotMask = (1u << kHotBits) - 1;
} // namespace

// QEMU/Unicorn ARM EXCP_* numbers passed to UC_HOOK_INTR.
const char* arm_excp_name(uint32_t intno) {
    switch (intno) {
        case 1:  return "UDEF (undefined instruction)";
        case 2:  return "SWI/SVC";
        case 3:  return "PREFETCH_ABORT (instruction abort)";
        case 4:  return "DATA_ABORT";
        case 5:  return "IRQ";
        case 6:  return "FIQ";
        case 7:  return "BKPT";
        case 11: return "HVC";
        case 13: return "SMC";
        default: return "exception";
    }
}

UnicornCpu::UnicornCpu(UnicornOptions opts) : opts_(opts) {
    hot_.assign(1u << kHotBits, 0);
}

UnicornCpu::~UnicornCpu() {
    if (csh_) { csh h = (csh)(uintptr_t)csh_; cs_close(&h); }
    if (uc_) uc_close(uc_);
}

// Device SPI interrupt delivery: bridge the device/GIC models to the patched
// Unicorn ICC path. Set once attach() creates the engine (single instance).
static uc_engine* g_irq_uc = nullptr;
static void hw_raise_irq(uint32_t intid, bool level) {
    static uint64_t n = 0;
    if (level && (++n % 200000 == 0)) HW_WARN("irq", "SPI {} raised {} times (storm?)", intid, n);
    if (g_irq_uc) uc_arm64_set_irq(g_irq_uc, intid, level ? 1 : 0);
}
static void hw_set_irq_enabled(uint32_t intid, bool en) { if (g_irq_uc) uc_arm64_set_irq_enabled(g_irq_uc, intid, en ? 1 : 0); }

bool UnicornCpu::attach(mem::GuestMemory& ram, dev::DeviceBus& bus, std::string& err) {
    ram_ = &ram;
    bus_ = &bus;

    // Phase 11: enable EL3 so the real Qualcomm secure monitor (tz) can run. The
    // backend's ARM core reads HOLLYWOOD_EL3 at realize time (cpu.c) to keep EL3 +
    // ARM_FEATURE_EL3 and to report a Qualcomm MIDR; must be set before uc_open.
    if (opts_.el3) {
        _putenv("HOLLYWOOD_EL3=1");
        HW_INFO("cpu.uc", "EL3 enabled (secure monitor / tz)");
        if (const char* t = std::getenv("HOLLYWOOD_TZ_TRAP"))
            tz_trap_pc_ = std::strtoull(t, nullptr, 0);
    }
    if (opts_.kmshim) {
        void hollywood_install_scm_shim();
        hollywood_install_scm_shim();
        HW_WARN("cpu.uc", "keymaster/QSEE SCM shim INSTALLED -- boot is NON-FAITHFUL past the secure world");
    }

    uc_err e = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc_);
    if (e != UC_ERR_OK) { err = std::string("uc_open: ") + uc_strerror(e); return false; }
    g_irq_uc = uc_;
    dev::install_irq_backend(hw_raise_irq, hw_set_irq_enabled);

    // TCG code-generation buffer size. The REAL fix for the long-boot host crash is
    // the code-cache RECYCLING fix in Unicorn (translate-all.c code_gen_buffer_handler:
    // the near-end on-demand-commit committed a full 4MB chunk PAST the reservation ->
    // VirtualAlloc failed -> the last ~4MB was never usable -> code-gen faulted there
    // ~4MB before the high-water mark, so tb_flush NEVER triggered and the cache never
    // recycled). With that fixed, tb_flush recycles correctly at ANY buffer size. We
    // set a moderate 512 MiB (vs the 1 GiB default) purely to reduce flush frequency
    // (fewer full re-translations); it is MEM_RESERVE + committed on demand, so it
    // costs host RAM only up to the working set. Set after uc_open, before emulation.
    uc_err be = uc_ctl_set_tcg_buffer_size(uc_, 0x20000000u);   // 512 MiB
    if (be != UC_ERR_OK)
        HW_WARN("cpu.uc", "uc_ctl_set_tcg_buffer_size failed ({}) -- using 1 GiB default", (int)be);

    // Select the most capable ARM64 core. The Quest kernel programs TCR_EL1 with
    // IPS=0b100 (44-bit PA / larger-address features); the default A57 model can
    // stall in stage-1 translation once the MMU is enabled. "max" supports the
    // full feature set QEMU-TCG implements.
    int model = UC_CPU_ARM64_MAX;
    uc_err ce = uc_ctl_set_cpu_model(uc_, model);
    if (ce != UC_ERR_OK)
        HW_WARN("cpu.uc", "uc_ctl_set_cpu_model(max) failed ({}) -- using default", (int)ce);
    else
        HW_INFO("cpu.uc", "CPU model = ARM64 'max'");

    if (opts_.host_backed_ram) {
        // Single source of truth: map the emulator's RAM buffer directly (no copy).
        e = uc_mem_map_ptr(uc_, ram.base(), (size_t)ram.size(), UC_PROT_ALL, ram.host_ptr(ram.base()));
        if (e != UC_ERR_OK) { err = std::string("uc_mem_map_ptr(RAM): ") + uc_strerror(e); return false; }
        HW_INFO("cpu.uc", "mapped RAM {:#x}+{:#x} (host-backed)", ram.base(), ram.size());
    } else {
        // Unicorn-owned RAM; copy the currently-loaded image in. (Diagnostic mode:
        // isolates suspected uc_mem_map_ptr page-table-walk issues.)
        e = uc_mem_map(uc_, ram.base(), (size_t)ram.size(), UC_PROT_ALL);
        if (e != UC_ERR_OK) { err = std::string("uc_mem_map(RAM): ") + uc_strerror(e); return false; }
        e = uc_mem_write(uc_, ram.base(), ram.host_ptr(ram.base()), (size_t)ram.size());
        if (e != UC_ERR_OK) { err = std::string("uc_mem_write(RAM): ") + uc_strerror(e); return false; }
        HW_INFO("cpu.uc", "mapped RAM {:#x}+{:#x} (unicorn-owned, copied)", ram.base(), ram.size());
    }

    // Route each device's MMIO window to the real device model.
    for (const auto& d : bus.devices()) {
        auto ctx = std::make_unique<MmioCtx>();
        ctx->self = this; ctx->dev = d.get(); ctx->base = d->base();
        e = uc_mmio_map(uc_, d->base(), (size_t)d->size(),
                        mmio_read_cb, ctx.get(), mmio_write_cb, ctx.get());
        if (e != UC_ERR_OK) {
            err = std::string("uc_mmio_map(") + d->name() + "): " + uc_strerror(e);
            return false;
        }
        HW_INFO("cpu.uc", "mapped MMIO {} {:#x}+{:#x}", d->name(), d->base(), d->size());
        mmio_ctxs_.push_back(std::move(ctx));
    }

    // Take over address translation. Unicorn's built-in AArch64 stage-1 walker
    // spuriously prefetch-aborts on valid mappings once the guest MMU is on; we
    // switch to the virtual-TLB mode and fill translations ourselves with a
    // correct ARMv8 page-table walk (see translate()).
    if (opts_.our_mmu) {
        uc_err te = uc_ctl_tlb_mode(uc_, UC_TLB_VIRTUAL);
        if (te != UC_ERR_OK) HW_WARN("cpu.uc", "uc_ctl_tlb_mode(VIRTUAL) failed ({})", (int)te);
        uc_hook th;
        uc_hook_add(uc_, &th, UC_HOOK_TLB_FILL, (void*)tlb_cb, this, 1, 0);
        // Flush our virtual TLB on TLBI (SYS instruction, CRn=8) so page-table
        // edits the guest makes (notably fixmap remaps) are never read stale.
        uc_hook sh;
        uc_hook_add(uc_, &sh, UC_HOOK_INSN, (void*)sys_cb, this, 1, 0, UC_ARM64_INS_SYS);
        // (GICv3 CPU-interface ICC_* registers are now provided natively by our
        // patched Unicorn cpreg table, so no MRS/MSR interception is needed.)
        HW_INFO("cpu.uc", "translation provided by built-in ARMv8 walker (UC_TLB_VIRTUAL)");
    }

    uc_hook h;
    if (opts_.code_hook) {
        // Fast path: a per-block hook carries instruction counting, the arch-timer
        // poll, spin detection and the heartbeat. The per-instruction UC_HOOK_CODE
        // (which forces TCG to instrument every instruction and is ~10-50x slower)
        // is only added when we actually need per-instruction visibility.
        uc_hook_add(uc_, &h, UC_HOOK_BLOCK, (void*)block_cb, this, 1, 0);
        const bool need_insn_hook = opts_.trace || !opts_.fn_trace_ksyms.empty();
        if (need_insn_hook)
            uc_hook_add(uc_, &h, UC_HOOK_CODE, (void*)code_cb, this, 1, 0);
        uc_hook_add(uc_, &h, UC_HOOK_MEM_UNMAPPED, (void*)unmapped_cb, this, 1, 0);
        uc_hook_add(uc_, &h, UC_HOOK_MEM_PROT, (void*)unmapped_cb, this, 1, 0);
        if (std::getenv("HOLLYWOOD_WATCH08C")) {   // Phase-9 table[10] write watchpoint
            uc_hook wh;
            uc_hook_add(uc_, &wh, UC_HOOK_MEM_WRITE, (void*)watch_cb, this, 1, 0);  // all addrs
        }
        uc_hook_add(uc_, &h, UC_HOOK_INTR, (void*)intr_cb, this, 1, 0);

        // SELinux permissive for the compat boot: a single-address code hook on
        // avc_denied() forces it to return 0 (allow). See selinux_permissive_cb.
        if (opts_.kmshim) {
            uc_hook ah;
            uc_hook_add(uc_, &ah, UC_HOOK_CODE, (void*)selinux_permissive_cb, this,
                        kAvcDeniedVA, kAvcDeniedVA);
            std::printf("[kmshim] SELinux permissive: avc_denied hook @ %#llx\n",
                        (unsigned long long)kAvcDeniedVA);
            uc_hook mrh;
            uc_hook_add(uc_, &mrh, UC_HOOK_CODE, (void*)machine_restart_cb, this,
                        kMachineRestartVA, kMachineRestartVA);
        }
    }

    // Disassembler for trace mode (best-effort).
    csh handle = 0;
    cs_err cse = cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle);
    if (cse == CS_ERR_OK) csh_ = (void*)(uintptr_t)handle;
    else HW_WARN("cpu.uc", "capstone cs_open failed ({}): disasm disabled", (int)cse);

    if (!opts_.fn_trace_ksyms.empty()) load_fn_trace();

    return true;
}

// Load the addresses of a fixed set of timer/irq functions from a ksyms.txt
// ("<hexaddr> <type> <name>" per line) so code_cb can trace their calls.
void UnicornCpu::load_fn_trace() {
    static const char* kWatch[] = {
        // exec -> ELF load -> initial-stack build -> start_thread (userspace ABI)
        "do_execveat_common", "__do_execve_file", "bprm_execve", "search_binary_handler",
        "load_elf_binary", "load_elf_interp", "elf_map", "set_brk",
        "create_elf_tables", "setup_arg_pages", "__bprm_mm_init", "copy_strings",
        "copy_strings_kernel", "begin_new_exec", "flush_old_exec", "setup_new_exec",
        "start_thread", "finalize_exec", "padzero",
    };
    std::FILE* f = std::fopen(opts_.fn_trace_ksyms.c_str(), "rb");
    if (!f) { HW_WARN("trace", "fn-trace: cannot open {}", opts_.fn_trace_ksyms); return; }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long addr; char type; char nm[256];
        if (std::sscanf(line, "%llx %c %255s", &addr, &type, nm) != 3) continue;
        for (const char* w : kWatch) if (std::strcmp(w, nm) == 0) { fn_watch_[addr] = nm; break; }
    }
    std::fclose(f);
    HW_WARN("trace", "fn-trace: watching {} function entries", fn_watch_.size());
}

void UnicornCpu::set_state(const CpuState& st) {
    for (int i = 0; i < 31; ++i) { uint64_t v = st.regs.x[i]; uc_reg_write(uc_, xreg(i), &v); }
    uint64_t sp = st.regs.sp, pc = st.regs.pc, pstate = st.regs.pstate;
    uc_reg_write(uc_, UC_ARM64_REG_SP, &sp);
    uc_reg_write(uc_, UC_ARM64_REG_PC, &pc);
    uc_reg_write(uc_, UC_ARM64_REG_PSTATE, &pstate);
    HW_INFO("cpu.uc", "state set: PC={:#x} X0={:#x} PSTATE={:#x}", pc, st.regs.x[0], pstate);
}

RunResult UnicornCpu::run(uint64_t max_instructions) {
    insns_ = 0; traced_ = 0; fault_.valid = false; spin_ = false; spin_pc_ = 0;
    exc_last_pc_ = 0; exc_last_no_ = 0; exc_repeat_ = 0; exc_storm_ = false;
    exc_vectored_ = 0; last_tlb_miss_ = 0; warns_skipped_ = 0;
    g_guest_reboot = false;
    std::memset(hot_.data(), 0, hot_.size() * sizeof(uint32_t));
    uc_arm64_time_reset(uc_);                 // deterministic virtual clock from 0
    uint64_t pc = 0; uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);

    if (opts_.trace) HW_INFO("cpu.uc", "tracing first {} instructions", opts_.trace_limit);
    auto t0 = std::chrono::steady_clock::now();
    uc_err e = UC_ERR_OK;
    uint64_t elapsed_us = 0;
    if (opts_.step) {
        // Single-step: one instruction per uc_emu_start (one TB each). Slower, but
        // it changes how QEMU regenerates the TB straddling the MMU-enable.
        for (uint64_t i = 0; i < max_instructions; ++i) {
            uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);
            if (opts_.code_hook == false) insns_++;   // count here if no per-insn hook
            e = uc_emu_start(uc_, pc, 0, 0, 1);
            if (e != UC_ERR_OK || spin_ || g_guest_reboot) break;
            elapsed_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (opts_.timeout_us && elapsed_us >= opts_.timeout_us) break;
        }
    } else {
        // Run with WFI/idle handling on a deterministic virtual clock. When the
        // guest executes WFI it has finished work and waits for an interrupt, so
        // uc_emu_start returns cleanly. We then warp virtual time to the next
        // armed timer deadline so its tick fires immediately (no real-time wait)
        // and resume -- sleep-heavy probing skips through instantly. If NO timer
        // is armed the guest is blocked on a device IRQ that cannot arrive while
        // idle: that is a genuine dead stall, surfaced as a spin.
        int dead_warps = 0;
        for (;;) {
            uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);
            uint64_t el = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (opts_.timeout_us && el >= opts_.timeout_us) break;
            if (insns_ >= max_instructions) break;
            uint64_t rem_to = opts_.timeout_us ? (opts_.timeout_us - el) : 0;
            uint64_t rem_ins = max_instructions - insns_;
            e = uc_emu_start(uc_, pc, 0, rem_to, rem_ins);
            if (e != UC_ERR_OK || spin_ || g_guest_reboot) break;   // stop / error / spin / reboot
            if (insns_ >= max_instructions) break;           // hit the instruction cap
            el = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (opts_.timeout_us && el >= opts_.timeout_us) break;   // timed out
            // WFI idle halt: fast-forward to the next timer deadline.
            if (uc_arm64_time_warp(uc_)) {
                dead_warps = 0;                              // a timer fired: progress
            } else if (++dead_warps >= 64) {
                // No timer armed across many idle returns -> deadlocked on a device
                // interrupt. Record it as a spin so diagnostics dump the state.
                spin_ = true; spin_pc_ = pc;
                HW_WARN("cpu.idle", "WFI dead stall at pc={:#x} (no timer armed; "
                        "guest waiting on a device IRQ that never arrives)", pc);
                break;
            }
        }
    }
    elapsed_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();

    RunResult r;
    r.instructions_executed = insns_;
    uc_reg_read(uc_, UC_ARM64_REG_PC, &r.pc);
    if (last_mmio_.valid) {
        r.last_mmio_valid = true;
        r.last_mmio_write = last_mmio_.is_write;
        r.last_mmio_addr = last_mmio_.addr;
        r.last_mmio_value = last_mmio_.value;
        r.last_mmio_pc = last_mmio_.pc;
    }
    if (fault_.valid) { r.fault_is_write = fault_.is_write; r.fault_size = fault_.size; }

    bool timed_out = opts_.timeout_us && elapsed_us >= opts_.timeout_us && insns_ < max_instructions;

    if (exc_storm_ || (e == UC_ERR_EXCEPTION)) {
        r.kind = RunResult::Kind::Exception;
        uint32_t no = exc_storm_ ? exc_storm_no_ : 0;
        if (exc_storm_) {
            r.pc = exc_storm_pc_;
            r.detail = std::string("repeated CPU exception: ") + arm_excp_name(no) +
                       " re-raised at the same PC (cannot be delivered -- e.g. VBAR not set)";
        } else {
            r.detail = std::string("unhandled CPU exception (") + uc_strerror(e) + ")";
        }
    } else if (spin_) {
        r.kind = RunResult::Kind::Spin;
        r.pc = spin_pc_;
        r.fault_addr = spin_pc_;
        r.detail = "hot loop / spin-wait detected (guest is busy-waiting on hardware state)";
    } else if (e != UC_ERR_OK && fault_.valid) {
        r.kind = RunResult::Kind::MemFault;
        r.fault_addr = fault_.addr;
        r.detail = std::string(uc_strerror(e)) + " (unclaimed guest memory access)";
    } else if (e != UC_ERR_OK) {
        r.kind = RunResult::Kind::Exception;
        r.detail = uc_strerror(e);
    } else if (insns_ >= max_instructions) {
        r.kind = RunResult::Kind::InsnLimit;
        r.detail = "reached instruction limit";
    } else if (timed_out) {
        r.kind = RunResult::Kind::Spin;
        r.detail = "wall-clock timeout (no fault, no halt -- likely a slow spin-wait)";
    } else {
        r.kind = RunResult::Kind::Halted;
        r.detail = "execution halted";
    }
    return r;
}

Aarch64Regs UnicornCpu::read_regs() {
    Aarch64Regs regs;
    for (int i = 0; i < 31; ++i) uc_reg_read(uc_, xreg(i), &regs.x[i]);
    uc_reg_read(uc_, UC_ARM64_REG_SP, &regs.sp);
    uc_reg_read(uc_, UC_ARM64_REG_PC, &regs.pc);
    uc_reg_read(uc_, UC_ARM64_REG_PSTATE, &regs.pstate);
    return regs;
}

bool UnicornCpu::read_mem(uint64_t addr, void* buf, size_t len) {
    if (uc_ && uc_mem_read(uc_, addr, buf, len) == UC_ERR_OK) return true;
    // Fall back to our own translation (uc_mem_read works on physical addrs; the
    // guest PC may be a kernel virtual address once the MMU is on).
    if (ram_) {
        uint64_t pa = 0;
        if (translate(addr, pa)) {
            if (ram_->contains(pa, len)) {
                std::memcpy(buf, ram_->host_ptr(pa), len);
                return true;
            }
            // Translated PA in a non-DRAM region (e.g. the tz secure carveout).
            if (uc_ && uc_mem_read(uc_, pa, buf, len) == UC_ERR_OK) return true;
        }
    }
    return false;
}

bool UnicornCpu::map_ram_region(uint64_t base, uint64_t size, std::string& err) {
    if (!uc_) { err = "map_ram_region: engine not attached"; return false; }
    uc_err e = uc_mem_map(uc_, base, (size_t)size, UC_PROT_ALL);
    if (e != UC_ERR_OK) {
        err = std::string("uc_mem_map(") + std::to_string(base) + "): " + uc_strerror(e);
        return false;
    }
    HW_INFO("cpu.uc", "mapped extra RAM {:#x}+{:#x}", base, size);
    return true;
}

bool UnicornCpu::write_phys(uint64_t addr, const uint8_t* data, size_t len) {
    if (!uc_ || len == 0) return uc_ != nullptr;
    return uc_mem_write(uc_, addr, data, len) == UC_ERR_OK;
}

// ---- Unicorn C callbacks ----

uint64_t UnicornCpu::mmio_read_cb(uc_engine* uc, uint64_t offset, unsigned size, void* user) {
    auto* ctx = static_cast<MmioCtx*>(user);
    UnicornCpu* self = ctx->self;
    uint64_t val = ctx->dev->read(offset, size);
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    self->last_mmio_ = { true, false, ctx->base + offset, val, size, pc };
    if (self->opts_.log_mmio)
        HW_INFO("mmio", "PC={:#x} READ  {:#x} size={} -> {:#x} [{}]",
                pc, ctx->base + offset, size, val, ctx->dev->name());
    return val;
}

void UnicornCpu::mmio_write_cb(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user) {
    auto* ctx = static_cast<MmioCtx*>(user);
    UnicornCpu* self = ctx->self;
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    self->last_mmio_ = { true, true, ctx->base + offset, value, size, pc };
    if (self->opts_.log_mmio)
        HW_INFO("mmio", "PC={:#x} WRITE {:#x} size={} value={:#x} [{}]",
                pc, ctx->base + offset, size, value, ctx->dev->name());
    ctx->dev->write(offset, value, size);
}

std::vector<uint64_t> UnicornCpu::recent_pcs() const {
    std::vector<uint64_t> out;
    for (size_t i = 0; i < kPcRing; ++i) {
        uint64_t v = pc_ring_[(pc_ring_pos_ + i) % kPcRing];
        if (v) out.push_back(v);
    }
    return out;
}

// Decode and print the initial userspace (EL0) state at the exec->EL0 handoff:
// registers, TLS, and the initial stack (argc/argv/envp/auxv) the kernel built.
// This is the reference we compare against Linux create_elf_tables()/start_thread.
void UnicornCpu::dump_el0_entry() {
    using ull = unsigned long long;
    auto rd = [&](int reg){ uint64_t v = 0; uc_reg_read(uc_, reg, &v); return v; };
    auto r8 = [&](uint64_t va, uint64_t& out){ return read_mem(va, &out, 8); };
    auto cstr = [&](uint64_t va) -> std::string {
        std::string s; for (int i = 0; i < 128; ++i) { uint8_t c = 0;
            if (!read_mem(va + i, &c, 1) || c == 0) break; s += (char)c; } return s; };
    auto at_name = [](uint64_t t) -> const char* { switch (t) {
        case 3: return "AT_PHDR"; case 4: return "AT_PHENT"; case 5: return "AT_PHNUM";
        case 6: return "AT_PAGESZ"; case 7: return "AT_BASE"; case 8: return "AT_FLAGS";
        case 9: return "AT_ENTRY"; case 11: return "AT_UID"; case 12: return "AT_EUID";
        case 13: return "AT_GID"; case 14: return "AT_EGID"; case 15: return "AT_PLATFORM";
        case 16: return "AT_HWCAP"; case 17: return "AT_CLKTCK"; case 23: return "AT_SECURE";
        case 25: return "AT_RANDOM"; case 26: return "AT_HWCAP2"; case 31: return "AT_EXECFN";
        case 33: return "AT_SYSINFO_EHDR"; case 51: return "AT_MINSIGSTKSZ"; default: return "AT_?"; } };

    uint64_t pc = rd(UC_ARM64_REG_PC), sp = rd(UC_ARM64_REG_SP), ps = rd(UC_ARM64_REG_PSTATE);
    std::printf("\n\x1b[1m=== EL0 ENTRY (exec -> userspace /init) ===\x1b[0m\n");
    std::printf("PC=%#llx  SP=%#llx  PSTATE=%#llx (EL%d %s)\n", (ull)pc, (ull)sp, (ull)ps,
                (int)((ps >> 2) & 3), (ps & 0x10) ? "AArch32" : "AArch64");
    std::printf("SP_EL0=%#llx  TPIDR_EL0=%#llx  TPIDRRO_EL0=%#llx\n",
                (ull)rd(UC_ARM64_REG_SP_EL0), (ull)rd(UC_ARM64_REG_TPIDR_EL0), (ull)rd(UC_ARM64_REG_TPIDRRO_EL0));
    std::printf("TTBR0_EL1=%#llx  TTBR1_EL1=%#llx  VBAR_EL1=%#llx\n",
                (ull)rd(UC_ARM64_REG_TTBR0_EL1), (ull)rd(UC_ARM64_REG_TTBR1_EL1), (ull)rd(UC_ARM64_REG_VBAR_EL1));
    for (int i = 0; i < 31; i += 3) {
        std::printf("  ");
        for (int j = i; j < i + 3 && j < 31; ++j) std::printf("X%-2d=%#018llx  ", j, (ull)rd(xreg(j)));
        std::printf("\n");
    }
    uint64_t argc = 0;
    if (!r8(sp, argc)) { std::printf("  (cannot read initial stack @ SP)\n"); return; }
    std::printf("Initial stack @ SP=%#llx  (16-byte aligned: %s)\n",
                (ull)sp, (sp & 15) ? "NO -- MISALIGNED" : "yes");
    std::printf("  argc = %llu\n", (ull)argc);
    uint64_t p = sp + 8;
    for (uint64_t i = 0; i < argc && i < 16; ++i) { uint64_t av = 0; r8(p + 8*i, av);
        std::printf("  argv[%llu] = %#llx \"%s\"\n", (ull)i, (ull)av, cstr(av).c_str()); }
    uint64_t e = p + 8 * (argc + 1);           // past argv + its NULL terminator
    std::printf("  envp:\n");
    for (int n = 0; n < 40; ++n) { uint64_t ev = 0; if (!r8(e, ev) || ev == 0) break;
        std::printf("    %#llx \"%s\"\n", (ull)ev, cstr(ev).c_str()); e += 8; }
    e += 8;                                     // past envp NULL
    std::printf("  auxv:\n");
    for (int n = 0; n < 40; ++n) { uint64_t t = 0, v = 0; if (!r8(e, t) || !r8(e + 8, v)) break;
        if (t == 0) { std::printf("    AT_NULL\n"); break; }
        std::printf("    %-16s = %#llx\n", at_name(t), (ull)v); e += 16; }
    std::fflush(stdout);
}

// Fast path: fires once per translated basic block (not per instruction), so
// TCG can run blocks natively. Carries instruction counting, the arch-timer
// poll (interrupts are checked at block boundaries anyway), spin detection and
// the heartbeat.
void UnicornCpu::block_cb(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    self->insns_ += size ? (size / 4) : 1;          // block size in bytes -> #insns (AArch64)
    g_live_insns.store(self->insns_, std::memory_order_relaxed);   // GUI live counters
    g_live_pc.store(address, std::memory_order_relaxed);

    // Phase 11 (tz mode): tz cold-boots at EL3 and eventually ERETs to the NON-secure
    // world (Linux). Catch that transition and stop so run_tz can inject the kernel
    // boot state; tz's EL3 setup (VBAR_EL3 SMC handler) + the launched secure OS stay
    // resident. IMPORTANT: tz also drops to SECURE EL1 (S-EL1) during its boot to run
    // the QSEE trusted OS -- SCR_EL3.NS distinguishes them (NS==1 => non-secure), so
    // we only inject on the real non-secure handoff and let secure-OS excursions run.
    if (self->opts_.el3 && self->tz_trap_pc_ && address == self->tz_trap_pc_ && !self->tz_trapped_) {
        self->tz_trapped_ = true;
        uint64_t x[31], sp = 0;
        for (int i = 0; i < 31; i++) uc_reg_read(uc, UC_ARM64_REG_X0 + i, &x[i]);
        uc_reg_read(uc, UC_ARM64_REG_SP, &sp);
        HW_WARN("cpu.tz", "TRAP at {:#x} insns={} SP={:#x}", address,
                (unsigned long long)self->insns_, (unsigned long long)sp);
        for (int i = 0; i < 30; i += 2)
            HW_WARN("cpu.tz", "   x{}={:#x}  x{}={:#x}", i, (unsigned long long)x[i],
                    i + 1, (unsigned long long)x[i + 1]);
        for (int k = 40; k >= 1; --k) {
            size_t idx = (self->pc_ring_pos_ + kPcRing - (size_t)k) % kPcRing;
            uint64_t p = self->pc_ring_[idx];
            HW_WARN("cpu.tz", "   [{}] {:#x}: {}", 40 - k, (unsigned long long)p, self->disasm_str(p));
        }
        uc_emu_stop(uc);
        return;
    }
    if (self->opts_.el3 && !self->tz_dropped_) {
        uint64_t ps = 0; uc_reg_read(uc, UC_ARM64_REG_PSTATE, &ps);
        unsigned el = (unsigned)((ps >> 2) & 3);
        if (el == 3) self->tz_seen_el3_ = true;
        else if (self->tz_seen_el3_ && el != self->tz_last_low_el_) {
            uc_arm64_cp_reg cp; std::memset(&cp, 0, sizeof cp);
            cp.op0 = 3; cp.op1 = 6; cp.crn = 1; cp.crm = 1; cp.op2 = 0;   // SCR_EL3
            uc_reg_read(uc, UC_ARM64_REG_CP_REG, &cp);
            self->tz_last_low_el_ = el;
            if (self->tz_drop_log_ < 40) {
                self->tz_drop_log_++;
                HW_WARN("cpu.tz", "EL3->EL{} drop #{}: NS={} target={:#x} SCR_EL3={:#x} insns={}",
                        el, self->tz_drop_log_, (unsigned)(cp.val & 1u), address,
                        (unsigned long long)cp.val, (unsigned long long)self->insns_);
            }
            if (cp.val & 1u) {                        // NS==1 -> non-secure handoff
                self->tz_dropped_ = true;
                self->tz_drop_pc_ = address;
                uc_emu_stop(uc);
                return;
            }
            // else: SECURE EL1 (QSEE trusted OS) -- let tz's secure world run.
        }
        if (el == 3) self->tz_last_low_el_ = 3;

        // Secure-world "settled" detector: tz/QSEE reach a `b .` self-loop once the
        // monitor + trusted OS have finished their linear cold boot (on real HW xbl
        // then drives the NON-secure boot and QSEE services SMCs). Treat that park as
        // the point to hand off: force SCR_EL3.NS=1 (so the injected kernel runs
        // non-secure and its SMCs trap EL1->EL3 monitor->QSEE) and signal run_tz.
        if (!self->tz_dropped_ && address == self->tz_last_block_) {
            uint32_t insn = 0;
            if (self->read_mem(address, &insn, 4) && insn == 0x14000000u) {
                HW_WARN("cpu.tz", "secure world parked (b .) at {:#x} insns={} -- preceding trace:",
                        address, (unsigned long long)self->insns_);
                for (int k = 24; k >= 1; --k) {
                    size_t idx = (self->pc_ring_pos_ + kPcRing - (size_t)k) % kPcRing;
                    uint64_t p = self->pc_ring_[idx];
                    HW_WARN("cpu.tz", "   {:#x}: {}", p, self->disasm_str(p));
                }
                // Disasm the functions the park calls -- to judge error vs done-idle.
                for (uint64_t f : { (uint64_t)0x7acfe41f8ull, (uint64_t)0x7acfe4958ull }) {
                    HW_WARN("cpu.tz", " -- callee {:#x}:", f);
                    for (uint64_t p = f; p < f + 0x34; p += 4)
                        HW_WARN("cpu.tz", "     {:#x}: {}", p, self->disasm_str(p));
                }
                // Force SCR_EL3.NS=1 so the kernel is injected into the non-secure world.
                uc_arm64_cp_reg cp; std::memset(&cp, 0, sizeof cp);
                cp.op0 = 3; cp.op1 = 6; cp.crn = 1; cp.crm = 1; cp.op2 = 0;   // SCR_EL3
                uc_reg_read(uc, UC_ARM64_REG_CP_REG, &cp);
                cp.val |= 1u;
                uc_reg_write(uc, UC_ARM64_REG_CP_REG, &cp);
                self->tz_dropped_ = true;
                self->tz_drop_pc_ = address;
                uc_emu_stop(uc);
                return;
            }
        }
        self->tz_last_block_ = address;
    }

    // --trace-user: watch the exec->EL0 handoff and the userspace ABI. A low
    // (user-half) VA executing once the MMU is up and PSTATE.EL==0 is userspace.
    if (self->opts_.trace_user) {
        // EL0 = PSTATE.EL (bits[3:2]) == 0. (Low VAs also execute at EL1 via the
        // idmap during TTBR/TLB switches, so the VA alone is not enough.)
        uint64_t ps = 0; uc_reg_read(uc, UC_ARM64_REG_PSTATE, &ps);
        const bool el0 = self->mmu_on_ && ((ps >> 2) & 3) == 0;
        if (el0) {
            if (!self->user_entered_) { self->user_entered_ = true; self->dump_el0_entry(); }
            if (self->user_traced_ < self->opts_.trace_user_insns) {
                self->user_traced_++;
                std::printf("  \x1b[36m[EL0]\x1b[0m %#010llx: %s\n",
                            (unsigned long long)address, self->disasm_str(address).c_str());
            }
        }
        // Syscall trace: an EL0 synchronous exception vectors to VBAR_EL1+0x400.
        if (self->user_entered_) {
            uint64_t vbar = 0; uc_reg_read(uc, UC_ARM64_REG_VBAR_EL1, &vbar);
            if (vbar && address == vbar + 0x400) {
                uint64_t esr = 0; uc_reg_read(uc, UC_ARM64_REG_ESR_EL1, &esr);
                uint32_t ec = (uint32_t)(esr >> 26);
                if (ec == 0x15) {   // SVC (AArch64 syscall)
                    uint64_t x8 = 0, elr = 0; uc_reg_read(uc, UC_ARM64_REG_X8, &x8);
                    uc_reg_read(uc, UC_ARM64_REG_ELR_EL1, &elr);
                    uint64_t a[6]; for (int i = 0; i < 6; ++i) uc_reg_read(uc, xreg(i), &a[i]);
                    self->user_svc_count_++;
                    std::printf("  \x1b[33m[SYSCALL #%llu]\x1b[0m x8=%llu  x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx  (from %#llx)\n",
                                (unsigned long long)x8, (unsigned long long)x8,
                                (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
                                (unsigned long long)a[3], (unsigned long long)a[4], (unsigned long long)a[5],
                                (unsigned long long)elr);
                } else {  // fault from EL0 (abort/undef) -> the crash
                    uint64_t far_addr = 0, elr = 0; uc_reg_read(uc, UC_ARM64_REG_FAR_EL1, &far_addr);
                    uc_reg_read(uc, UC_ARM64_REG_ELR_EL1, &elr);
                    uint32_t raw = 0; self->read_mem(elr, &raw, 4);
                    std::printf("  \x1b[31m[EL0 EXCEPTION]\x1b[0m EC=%#x FAR=%#llx ELR(userPC)=%#llx ESR=%#llx  insn=%08x  %s\n",
                                ec, (unsigned long long)far_addr, (unsigned long long)elr, (unsigned long long)esr,
                                raw, (ec == 0 ? self->disasm_str(elr).c_str() : ""));
                    if (ec == 0) {  // fatal undef at EL0 -> show how control flow got here
                        std::printf("    recent EL0/kernel blocks (oldest->newest):\n");
                        auto ring = self->recent_pcs();
                        for (size_t i = (ring.size() > 20 ? ring.size() - 20 : 0); i < ring.size(); ++i)
                            std::printf("      %#llx  %s\n", (unsigned long long)ring[i],
                                        self->disasm_str(ring[i]).c_str());
                        std::fflush(stdout);
                    }
                }
                std::fflush(stdout);
            }
        }
    }

    // kmshim diagnostic: catch a userspace process exiting with a nonzero status
    // (e.g. vendor.qseecomd exit 255) and dump its recent userspace PCs, so we can
    // see WHERE it failed (its stderr/logcat isn't captured). An EL0 SVC vectors to
    // VBAR_EL1+0x400 with x8=syscall, x0=arg0; exit(93)/exit_group(94) carry status.
    if (self->opts_.kmshim) {
        // If a prior ssd openat set a pending return-PC, this block IS the userspace
        // instruction right after that SVC -> x0 now holds the syscall result (fd >= 0
        // on success, or a negative errno like -2=ENOENT if the by-name/ssd symlink
        // doesn't exist yet). Definitively tells us WHY qseecomd's ssd open fails.
        if (self->km_ssd_ret_ && address == self->km_ssd_ret_) {
            self->km_ssd_ret_ = 0;
            uint64_t rx0 = 0; uc_reg_read(uc, UC_ARM64_REG_X0, &rx0);
            if (self->km_ssd_ret_log_ < 60) { self->km_ssd_ret_log_++;
                std::printf("[kmshim]   -> x0=%lld (%s)\n", (long long)(int64_t)rx0,
                            (int64_t)rx0 >= 0 ? "OPENED ok" : "FAILED (neg=errno)");
                std::fflush(stdout);
            }
        }
        uint64_t vbar = 0; uc_reg_read(uc, UC_ARM64_REG_VBAR_EL1, &vbar);
        if (vbar && address == vbar + 0x400) {
            uint64_t esr = 0; uc_reg_read(uc, UC_ARM64_REG_ESR_EL1, &esr);
            if ((esr >> 26) == 0x15) {                 // SVC (AArch64 syscall)
                uint64_t x8 = 0, a0 = 0, a1 = 0, a2 = 0;
                uc_reg_read(uc, UC_ARM64_REG_X8, &x8);
                uc_reg_read(uc, UC_ARM64_REG_X0, &a0);
                uc_reg_read(uc, UC_ARM64_REG_X1, &a1);
                uc_reg_read(uc, UC_ARM64_REG_X2, &a2);
                // record into the syscall ring
                self->km_sc_[self->km_sc_pos_] = { (uint32_t)x8, a0, a1, a2 };
                self->km_sc_pos_ = (self->km_sc_pos_ + 1) % 48;
                // openat(56) with O_SYNC (0x101000): the pattern qseecomd loops on
                // before exit(255) -- capture the path it's trying to open.
                // O_SYNC opens only (cheap: no path read for the common non-O_SYNC opens)
                // -- this is the qseecomd secure-storage pattern (by-name/ssd). Logs the
                // path + captures the syscall's errno (fd>=0 = opened, <0 = -errno).
                // Log opens on the QSEE/keymaster TA-load path (ssd + the TA image files
                // + /dev/qseecom) and capture each one's return value (fd>=0 / -errno).
                // This reveals WHERE libQSEEComAPI looks for the keymaster64 TA image and
                // whether it's found -- the Phase 20 question (why APP_START never fires).
                if (x8 == 56 && self->km_open_log_ < 300) {
                    char path[192] = {0};
                    self->read_mem(a1, path, sizeof(path) - 1);
                    path[sizeof(path) - 1] = 0;
                    bool ta = !std::strstr(path, "/proc/") && (
                              std::strstr(path, "keymaster") || std::strstr(path, "cmnlib") ||
                              std::strstr(path, ".mdt") || std::strstr(path, ".mbn") ||
                              std::strstr(path, "qseecom") || std::strstr(path, "/ssd") ||
                              std::strstr(path, "keystore") || std::strstr(path, "firmware_mnt") ||
                              std::strstr(path, "firmware/image") || std::strstr(path, "by-name/keymaster") ||
                              std::strstr(path, "by-name/cmnlib") ||
                              (a2 & 0x101000u) == 0x101000u /*O_SYNC (ssd)*/ );
                    if (ta) {
                        self->km_open_log_++;
                        std::printf("[kmshim] openat '%s' flags=%#llx\n", path,
                                    (unsigned long long)a2);
                        std::fflush(stdout);
                        uint64_t elr = 0; uc_reg_read(uc, UC_ARM64_REG_ELR_EL1, &elr);
                        self->km_ssd_ret_ = elr;   // capture the openat return value
                    }
                }
                // execve(221)/execveat(281): log the binary being launched. A crash-
                // looping service execs the same path repeatedly, so the recurring path
                // identifies the exit(25) process without guessing from the service name.
                if ((x8 == 221 || x8 == 281) && self->km_exec_log_ < 250) {
                    self->km_exec_log_++;
                    uint64_t patp = (x8 == 221) ? a0 : a1;   // execveat: pathname is arg1
                    char path[192] = {0};
                    self->read_mem(patp, path, sizeof(path) - 1);
                    path[sizeof(path) - 1] = 0;
                    uint64_t ttbr = 0; uc_reg_read(uc, UC_ARM64_REG_TTBR0_EL1, &ttbr);
                    std::printf("[kmshim] execve '%s' (ttbr0=%#llx)\n", path, (unsigned long long)ttbr);
                    std::fflush(stdout);
                }
                if ((x8 == 93 || x8 == 94) && a0 != 0 && self->km_exit_log_ < 60) {
                    self->km_exit_log_++;
                    uint64_t ttbr = 0; uc_reg_read(uc, UC_ARM64_REG_TTBR0_EL1, &ttbr);
                    std::printf("[kmshim] EL0 exit(status=%lld) ttbr0=%#llx -- recent syscalls (num a0 a1 a2), oldest first:\n",
                                (long long)(int32_t)a0, (unsigned long long)ttbr);
                    for (int k = 0; k < 48; k++) {
                        const auto& s = self->km_sc_[(self->km_sc_pos_ + k) % 48];
                        if (s.num || s.a0 || s.a1)
                            std::printf("   sc %-4u  %#llx %#llx %#llx\n", s.num,
                                        (unsigned long long)s.a0, (unsigned long long)s.a1,
                                        (unsigned long long)s.a2);
                    }
                    std::fflush(stdout);
                }
            }
        }
    }

    self->pc_ring_[self->pc_ring_pos_] = address;
    self->pc_ring_pos_ = (self->pc_ring_pos_ + 1) % kPcRing;

    // Advance deterministic virtual time by the block's instructions and refresh
    // the arch-timer output + CPU IRQ line (interrupts are taken at block edges).
    uc_arm64_time_tick(uc, self->insns_);

    // Spin-wait detection (windowed): a real spin dominates a short window of
    // execution; legitimate long loops complete and move on. Reset the histogram
    // per window so cumulative counts over a long boot don't false-positive.
    if (self->opts_.hot_threshold) {
        uint64_t win = self->insns_ / kSpinWindow;
        if (win != self->last_win_) {
            self->last_win_ = win;
            std::memset(self->hot_.data(), 0, self->hot_.size() * sizeof(uint32_t));
        }
        uint32_t& bucket = self->hot_[(uint32_t)(address >> 2) & kHotMask];
        if (++bucket >= (uint32_t)self->opts_.hot_threshold && !self->spin_) {
            self->spin_ = true;
            self->spin_pc_ = address;
            uc_emu_stop(uc);
            return;
        }
    }

    if (self->opts_.heartbeat) {
        uint64_t hb = self->insns_ / self->opts_.heartbeat;
        if (hb != self->last_hb_) {
            self->last_hb_ = hb;
            std::printf("  \x1b[2m[cpu]\x1b[0m executed %lluM insns, PC=%#llx\n",
                        (unsigned long long)(self->insns_ / 1000000), (unsigned long long)address);
            std::fflush(stdout);
        }
    }
}

// Per-instruction hook: only registered when tracing (--trace) or symbol
// function-tracing (--trace-irq) is active. Does NOT count instructions (the
// block hook owns the count) to avoid double-counting.
void UnicornCpu::code_cb(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);

    if (self->opts_.trace && self->traced_ < self->opts_.trace_limit) {
        self->traced_++;
        self->disasm_line(address);
    }

    // Symbol-based function tracing: log entry (args) and return value (x0).
    if (!self->fn_watch_.empty() && self->fn_trace_lines_ < 4000) {
        auto it = self->fn_watch_.find(address);
        if (it != self->fn_watch_.end()) {
            uint64_t x0=0,x1=0,x2=0,lr=0;
            uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
            uc_reg_read(uc, UC_ARM64_REG_X1, &x1);
            uc_reg_read(uc, UC_ARM64_REG_X2, &x2);
            uc_reg_read(uc, UC_ARM64_REG_LR, &lr);
            HW_WARN("trace", "{}> {}(x0={:#x} x1={:#x} x2={:#x}) lr={:#x}",
                    std::string(self->fn_retstk_.size()*2, ' '), it->second, x0, x1, x2, lr);
            self->fn_retstk_.push_back({lr, it->second});
            self->fn_trace_lines_++;
        }
        while (!self->fn_retstk_.empty() && address == self->fn_retstk_.back().first) {
            uint64_t x0=0; uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
            auto nm = self->fn_retstk_.back().second; self->fn_retstk_.pop_back();
            HW_WARN("trace", "{}< {} = {:#x}", std::string(self->fn_retstk_.size()*2, ' '), nm, x0);
            self->fn_trace_lines_++;
        }
    }
}

bool UnicornCpu::unmapped_cb(uc_engine* uc, int type, uint64_t address, int size, int64_t value, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    bool is_write = (type == UC_MEM_WRITE_UNMAPPED || type == UC_MEM_WRITE_PROT);
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    self->fault_ = { true, is_write, address, (uint64_t)value, (unsigned)size, pc };

    const char* kind = "UNMAPPED";
    if (type == UC_MEM_READ_PROT || type == UC_MEM_WRITE_PROT || type == UC_MEM_FETCH_PROT) kind = "PROT";
    else if (type == UC_MEM_FETCH_UNMAPPED) kind = "FETCH-UNMAPPED";
    HW_WARN("mmio", "PC={:#x} {} {} {:#x} size={}{}", pc, kind, is_write ? "WRITE" : "READ",
            address, size, is_write ? "" : "");

    // Phase 11 diagnostic: when the secure world reads an unmodeled chip-id/fuse
    // register, disasm the check code that follows so we learn the expected value.
    // Covers TCSR SoC-HW-version (0x1fc8000), QFPROM (0x780000-0x790000), and the
    // early monitor/QSEE config reads. One-shot per site (capped).
    if (self->opts_.el3 && !is_write &&
        (address == 0x1fc8000ull || (address >= 0x780000ull && address < 0x790000ull) ||
         address == 0xc2f0000ull || (address >= 0x4fc000ull && address < 0x4fe000ull))) {
        static int dis_n = 0;
        if (dis_n < 8) { dis_n++;
            HW_WARN("cpu.tz", "secure-HW read {:#x} at PC={:#x} -- following check:", address, pc);
            for (uint64_t p = pc; p < pc + 0x2c; p += 4)
                HW_WARN("cpu.tz", "   {:#x}: {}", p, self->disasm_str(p));
        }
    }

    if (self->opts_.stop_on_unmapped) return false;   // stop -> surface the blocker

    // Permissive mode: back the page with zero RAM and continue.
    uint64_t page = address & ~0xfffull;
    uc_mem_map(uc, page, 0x1000, UC_PROT_ALL);
    return true;
}

void UnicornCpu::watch_cb(uc_engine* uc, int, uint64_t addr, int, int64_t value, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    if (self->insns_ < 9000000000ull) return;          // only near the boringssl process
    uint32_t v = (uint32_t)value;
    // Sample raw write addresses (to see if the hook reports VA or PA).
    static int raw = 0; if (raw < 12) { raw++;
        HW_WARN("watch08c", "raw write addr={:#x} val={:#x}", addr, v); }
    // The unrelocated value or the table page (VA) or its likely PA target.
    if (v == 0x881ed || v == 0x881ec || (addr & ~0xfffull) == 0xf7a58000ull) {
        static int n = 0; if (n >= 200) return; n++;
        uint64_t es[21] = {0}; uc_arm64_exec_state(uc, es);
        HW_WARN("watch08c", "HIT [{:#x} off{:#x}] = {:#x} aa32={} pc={:#x}",
                addr, addr & 0xfffull, v, es[0], es[2]);
    }
}

void UnicornCpu::intr_cb(uc_engine* uc, uint32_t intno, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    // The exception is now delivered to the guest's vector table natively (our
    // patched cpu_handle_exception calls do_interrupt, which sets SPSR/ELR/ESR/
    // FAR/PSTATE correctly). This hook is observe-only, plus a safety net: if the
    // SAME exception re-raises at the SAME PC an absurd number of times, a guest
    // handler is wedged -- stop rather than spin forever.
    uint64_t pc = 0; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    if (self->opts_.trace || self->opts_.log_mmio)
        HW_INFO("cpu.uc", "exception intno={} ({}) PC={:#x}", intno, arm_excp_name(intno), pc);

    // Debug: catch the first undefined-instruction at the moment it occurs, so
    // the recent-PC trace shows how control flow got there (before the oops).
    if (intno == 1 && self->opts_.stop_on_undef && !self->exc_storm_) {
        self->exc_storm_ = true; self->exc_storm_pc_ = pc; self->exc_storm_no_ = intno;
        uc_emu_stop(uc);
        return;
    }

    if (pc == self->exc_last_pc_ && intno == self->exc_last_no_) self->exc_repeat_++;
    else self->exc_repeat_ = 0;
    self->exc_last_pc_ = pc; self->exc_last_no_ = intno;
    if (self->exc_repeat_ > 4000000 && !self->exc_storm_) {
        self->exc_storm_ = true;
        self->exc_storm_pc_ = pc;
        self->exc_storm_no_ = intno;
        uc_emu_stop(uc);
    }
}

std::string UnicornCpu::disasm_str(uint64_t pc) {
    uint8_t code[4] = {};
    if (!read_mem(pc, code, 4)) return "<unreadable>";
    if (csh_) {
        cs_insn* insn = nullptr;
        size_t n = cs_disasm((csh)(uintptr_t)csh_, code, 4, pc, 1, &insn);
        if (n > 0) {
            std::string s = std::string(insn[0].mnemonic) + " " + insn[0].op_str;
            cs_free(insn, n);
            return s;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), ".word 0x%02x%02x%02x%02x", code[3], code[2], code[1], code[0]);
    return buf;
}

std::string UnicornCpu::disasm_at(uint64_t pc) { return disasm_str(pc); }

MmuRegs UnicornCpu::read_mmu_regs() {
    MmuRegs m; m.valid = true;
    uc_reg_read(uc_, UC_ARM64_REG_TTBR0_EL1, &m.ttbr0);
    uc_reg_read(uc_, UC_ARM64_REG_TTBR1_EL1, &m.ttbr1);
    uc_reg_read(uc_, UC_ARM64_REG_MAIR_EL1, &m.mair);
    uc_reg_read(uc_, UC_ARM64_REG_VBAR_EL1, &m.vbar);
    uc_reg_read(uc_, UC_ARM64_REG_ESR_EL1, &m.esr);
    uc_reg_read(uc_, UC_ARM64_REG_FAR_EL1, &m.far_el1);
    return m;
}

bool UnicornCpu::translate(uint64_t vaddr, uint64_t& paddr) {
    if (!ram_) { paddr = vaddr; return true; }
    // Phase 11: when the secure world is present, the EL3 monitor runs with its own
    // (MMU-off / identity) regime, NOT the EL1 page tables our walker reads below.
    // Once the kernel enables its MMU (mmu_on_), a kernel SMC that traps to the EL3
    // monitor would otherwise mis-walk TTBR*_EL1 for the monitor's physical code and
    // fault. At EL3, identity-map (the Qualcomm monitor runs physical). (S-EL1 QSEE
    // keeps its own tables and is handled by the normal TTBR0 walk below.)
    if (opts_.el3) {
        uint64_t ps = 0; uc_reg_read(uc_, UC_ARM64_REG_PSTATE, &ps);
        if (((ps >> 2) & 3) == 3) { paddr = vaddr; xlat_perms_ = UC_PROT_ALL; return true; }
    }
    // Apply TBI (Top-Byte-Ignore): arm64 Linux enables TCR_EL1.TBI0/TBI1, so the
    // MMU ignores VA bits[63:56] for translation. bionic tags heap pointers in the
    // top byte (e.g. 0xb4..); without stripping it a tagged user pointer has bit 63
    // set, looks like a kernel VA, mis-walks TTBR1 and faults forever (the kernel
    // untags and sees the page present -> spurious-fault loop). Reconstruct the
    // effective address by sign-extending bit 55.
    if (vaddr & (1ull << 55)) vaddr |= 0xff00000000000000ull;
    else                      vaddr &= 0x00ffffffffffffffull;
    uint64_t ttbr0 = 0, ttbr1 = 0;
    uc_reg_read(uc_, UC_ARM64_REG_TTBR0_EL1, &ttbr0);
    uc_reg_read(uc_, UC_ARM64_REG_TTBR1_EL1, &ttbr1);
    const bool high = (vaddr >> 63) & 1;                       // kernel-half VAs -> TTBR1
    if (high) mmu_on_ = true;   // high VAs only exist once the MMU + kernel page tables are up
    uint64_t table = (high ? ttbr1 : ttbr0) & 0x0000fffffffff000ull;

    if (table != 0) {
        const int shift[3] = { 30, 21, 12 };                  // L1/L2/L3 (VA39, 4KB)
        uint64_t t = table;
        for (int i = 0; i < 3; ++i) {
            uint64_t idx = (vaddr >> shift[i]) & 0x1ff;
            uint64_t da = t + idx * 8;
            // Read the descriptor from any mapped physical region. Page tables live
            // in main DRAM for the kernel, but the secure world (tz/QSEE) keeps its
            // tables in the tz carveout (outside GuestMemory), so fall back to the
            // unicorn physical space for those.
            uint64_t desc;
            if (ram_->contains(da, 8)) desc = ram_->read64(da);
            else if (!uc_ || uc_mem_read(uc_, da, &desc, 8) != UC_ERR_OK) break;
            if ((desc & 1) == 0) break;                       // invalid -> miss
            uint64_t next = desc & 0x0000fffffffff000ull;
            // Stage-1 permissions from the descriptor: AP[2] (bit 7) == 1 means
            // read-only, so a store must fault (this is what makes copy-on-write
            // work -- e.g. a write to a page still mapped to the shared zero page
            // vectors to the kernel, which allocates a private copy). READ/EXEC are
            // kept permissive; only WRITE is gated (the COW-critical bit).
            auto perms_of = [](uint64_t d) -> uint32_t {
                uint32_t p = UC_PROT_READ | UC_PROT_EXEC;
                if (((d >> 7) & 1) == 0) p |= UC_PROT_WRITE;   // AP[2]==0 -> writable
                return p;
            };
            if ((desc & 3) == 1) {                            // block
                uint64_t mask = (i == 0) ? 0x3fffffffull : 0x1fffffull;
                paddr = (next & ~mask) | (vaddr & mask);
                xlat_perms_ = perms_of(desc); return true;
            }
            if (i == 2) { paddr = next | (vaddr & 0xfffull);  // L3 page
                xlat_perms_ = perms_of(desc); return true; }
            t = next;
        }
    }
    // Linear-map (PAGE_OFFSET) fallback. The arm64 linear map is a fixed-offset
    // alias of all RAM: VA = PA - PHYS_OFFSET + PAGE_OFFSET (VA39: PAGE_OFFSET =
    // 0xffffffc000000000). The kernel writes patched code through lm_alias() of
    // .init.text during "alternatives: patching kernel code"; that page is not
    // present in the page-table walk (map_mem leaves the init image out of the
    // linear map), yet the guest expects the alias to reach the physical page.
    // Resolve it directly so self-patching lands on the real RAM bytes.
    constexpr uint64_t kLinearBase = 0xffffffc000000000ull;   // VA39 PAGE_OFFSET
    if (vaddr >= kLinearBase && vaddr < kLinearBase + ram_->size()) {
        paddr = ram_->base() + (vaddr - kLinearBase);
        xlat_perms_ = UC_PROT_ALL;   // kernel linear map (RW, self-patch)
        return true;
    }
    // Kernel-half miss => real translation fault. Low-half miss: once the MMU is
    // on, an unmapped user VA is a real fault (so EL0 demand-paging works -- e.g.
    // /init's pages fault in on first access); only before the MMU comes up do we
    // identity-map low addresses (pre-MMU / idmap physical accesses).
    if (high) return false;
    if (mmu_on_) {
        // AArch32 EL0 (32-bit userspace) faults are rare and the focus of Phase 9,
        // so log them separately (with the real AArch32 PC) and uncapped-ish.
        uint64_t es[21] = {0}; uc_arm64_exec_state(uc_, es);   // [5..20]=r0..r15
        if (es[0] /*is_aa32*/ && es[1] == 0 /*EL0*/) {
            static int aa32_log = 0;
            if (aa32_log < 120) { aa32_log++;
                HW_WARN("cpu.uc", "AA32-EL0 fault VA={:#x} PC={:#x} LR={:#x} SP={:#x}",
                        vaddr, es[2], es[3], es[4]); }
            // A code-fetch fault at a suspiciously low VA (< 1MB) is the fatal bad
            // jump; dump the caller's Thumb code + stack to see how the bad target
            // was formed (once).
            static bool dumped = false;
            if (!dumped && (vaddr >> 12) == (es[2] >> 12) && vaddr < 0x100000) {
                dumped = true;
                // Read the PROCESS's virtual memory via our own TTBR0 walk (uc_mem_read
                // hits physical + permissive zero pages, so it can't see userspace).
                uint32_t saved_perms = xlat_perms_;
                auto read_va = [&](uint64_t va, void* b, size_t n) -> bool {
                    uint64_t pa = 0;
                    if (translate(va, pa) && ram_ && ram_->contains(pa, n)) {
                        std::memcpy(b, ram_->host_ptr(pa), n); return true; }
                    return false;
                };
                uint64_t lr = es[3] & ~1ull;                 // strip Thumb bit
                uint64_t start = (lr > 96) ? ((lr - 96) & ~1ull) : 0;
                uint8_t code[100] = {};
                csh th; bool arm_ok = (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &th) == CS_ERR_OK);
                HW_WARN("cpu.uc", "AA32 FATAL blx->{:#x} LR={:#x} (capstone-ARM={})", es[2], lr, arm_ok?"ok":"UNAVAILABLE");
                if (read_va(start, code, sizeof code)) {
                    if (arm_ok) {
                        cs_insn* insn = nullptr;
                        size_t n = cs_disasm(th, code, sizeof code, start, 0, &insn);
                        for (size_t i = 0; i < n; ++i)
                            HW_WARN("cpu.uc", "  AA32 [{:#x}] {} {}",
                                    (uint64_t)insn[i].address, insn[i].mnemonic, insn[i].op_str);
                        if (insn) cs_free(insn, n);
                    }
                    std::string h;                            // raw halfwords for hand-decode
                    for (int i = 0; i < (int)sizeof(code); i += 2) { uint16_t w; std::memcpy(&w,code+i,2); char c[8]; std::snprintf(c,8,"%04x ",w); h += c; }
                    HW_WARN("cpu.uc", "  AA32 caller halfwords @[{:#x}]: {}", start, h);
                } else HW_WARN("cpu.uc", "  caller code @{:#x} unreadable", start);
                if (arm_ok) cs_close(&th);
                // r5 = ldr.w r5,[r4,r7,lsl#2] -> function-pointer table at r4, index
                // r7 (from live registers, ASLR-independent). Read the table to see
                // whether the bad entry is 0x881ed *in memory* (unrelocated =>
                // reloc/write bug) or the load mis-executed (Unicorn AArch32 bug).
                uint64_t r4 = es[5 + 4], r7 = es[5 + 7], r5 = es[5 + 5];
                uint64_t entry_va = r4 + r7 * 4;
                HW_WARN("cpu.uc", "  AA32 regs: r4={:#x} r5={:#x} r7={:#x} -> table[{}] @ {:#x}", r4, r5, r7, r7, entry_va);
                uint8_t tab[80] = {};
                uint64_t tbase = (r4 > 32) ? r4 - 32 : r4;
                if (read_va(tbase, tab, 80)) {
                    std::string h;
                    for (int i = 0; i < 80; i += 4) { uint32_t w; std::memcpy(&w,tab+i,4); char c[12]; std::snprintf(c,12,"%08x ",w); h += c; }
                    HW_WARN("cpu.uc", "  AA32 table @[{:#x}] (r4-32): {}", tbase, h);
                }
                uint8_t stk[48] = {};
                if (read_va(es[4], stk, 48)) {
                    std::string h;
                    for (int i = 0; i < 48; i += 4) { uint32_t w; std::memcpy(&w,stk+i,4); char c[12]; std::snprintf(c,12,"%08x ",w); h += c; }
                    HW_WARN("cpu.uc", "AA32 FATAL stack @SP [{:#x}]: {}", es[4], h);
                    // The 3 args were loaded from *(sp+16/20/24): a per-test struct
                    // in libcrypto .data. Dump that region to see whether the func
                    // pointer stored there is relocated (0xf7xxxxxx) or still an
                    // unrelocated file offset (~0x00xxxxxx) => distinguishes a bad
                    // relocation from a runtime register corruption.
                    uint32_t argptr; std::memcpy(&argptr, stk + 16, 4);   // *(sp+16)
                    uint64_t tbl = (uint64_t)(argptr & ~0x3full);
                    uint8_t td[128] = {};
                    if (tbl && read_va(tbl, td, 128)) {
                        std::string h2;
                        for (int i = 0; i < 128; i += 4) { uint32_t w; std::memcpy(&w,td+i,4); char c[12]; std::snprintf(c,12,"%08x ",w); h2 += c; }
                        HW_WARN("cpu.uc", "AA32 FATAL .data @[{:#x}]: {}", tbl, h2);
                    }
                }
                xlat_perms_ = saved_perms;
            }
        } else {
            static int lo_miss_log = 0;
            if (lo_miss_log < 200) { lo_miss_log++;
                uint64_t pc = 0; uc_reg_read(uc_, UC_ARM64_REG_PC, &pc);
                HW_WARN("cpu.uc", "EL0/low fault VA={:#x} at PC={:#x}", vaddr, pc); }
        }
        return false;
    }
    paddr = vaddr; xlat_perms_ = UC_PROT_ALL; return true;   // pre-MMU identity
}

uint32_t UnicornCpu::sys_cb(uc_engine* uc, int /*reg*/, const void* cp_reg, void* /*user*/) {
    const auto* cp = static_cast<const uc_arm64_cp_reg*>(cp_reg);
    if (cp && cp->crn == 8)          // TLBI (CRn=8): a translation just changed
        uc_ctl_flush_tlb(uc);
    return 0;                        // let the instruction execute normally
}

namespace {
// Is this system register a GICv3 CPU interface (ICC_*) register at EL1?
inline bool is_icc(const uc_arm64_cp_reg* cp) {
    return cp && cp->op0 == 3 && cp->op1 == 0 &&
           (cp->crn == 12 || (cp->crn == 4 && cp->crm == 6));
}
inline uint32_t icc_key(const uc_arm64_cp_reg* cp) {
    return (cp->crn << 8) | (cp->crm << 4) | cp->op2;
}
} // namespace

uint32_t UnicornCpu::mrs_cb(uc_engine* uc, int reg, void* cp_reg, void* user) {
    auto* cp = static_cast<uc_arm64_cp_reg*>(cp_reg);
    if (!is_icc(cp)) return 0;                        // not ICC: execute normally
    auto* self = static_cast<UnicornCpu*>(user);
    uint32_t key = icc_key(cp);
    uint64_t val = self->icc_.count(key) ? self->icc_[key] : 0;
    if (cp->crn == 12 && cp->crm == 12 && cp->op2 == 5) val |= 0x1;         // ICC_SRE_EL1.SRE
    if (cp->crn == 12 && cp->op2 == 0 && (cp->crm == 12 || cp->crm == 8)) val = 1023; // IAR: spurious
    if (cp->crn == 12 && cp->crm == 11 && cp->op2 == 3) val = 0xff;          // ICC_RPR_EL1 idle
    uc_reg_write(uc, reg, &val);
    return 1;                                         // handled: skip the real MRS
}

uint32_t UnicornCpu::msr_cb(uc_engine* uc, int reg, void* cp_reg, void* user) {
    auto* cp = static_cast<uc_arm64_cp_reg*>(cp_reg);
    if (!is_icc(cp)) return 0;                        // not ICC: execute normally
    auto* self = static_cast<UnicornCpu*>(user);
    uint64_t val = 0;
    uc_reg_read(uc, reg, &val);                       // value being written (source Xt)
    self->icc_[icc_key(cp)] = val;
    return 1;                                         // handled: skip the real MSR
}

bool UnicornCpu::tlb_cb(uc_engine*, uint64_t vaddr, int type, void* result, void* user) {
    auto* self = static_cast<UnicornCpu*>(user);
    auto* e = static_cast<uc_tlb_entry*>(result);
    uint64_t pa = 0;
    self->xlat_perms_ = UC_PROT_ALL;
    if (!self->translate(vaddr, pa)) { self->last_tlb_miss_ = vaddr; return false; }  // fault
    e->paddr = pa;
    // Real stage-1 permissions (from the PTE): a write to a read-only page then
    // fails the vtlb's perms check and vectors a permission fault to the guest,
    // enabling copy-on-write. Without this every page was writable (UC_PROT_ALL),
    // so writes to the shared zero page corrupted it for all mappings.
    e->perms = (uc_prot)self->xlat_perms_;
    // --- Phase 9 live probe (TEMPORARY; remove after): watch the linker's own
    // .init_array page (vaddr 0xf7a58000 = bias 0xf7982000 + 0xd6000). Logs every
    // fill of that page: access type, physical backing, PTE perms, and the physical
    // values of table[9/10/11]. type==UC_MEM_WRITE with perms lacking WRITE is the
    // copy-on-write fault we are trying to confirm. Non-perturbing (fill already runs). */
    static const bool relr_probe = std::getenv("HOLLYWOOD_RELR_PROBE") != nullptr;
    if (relr_probe) {
        uint32_t t10 = 0;
        uc_mem_read(self->uc_, pa + 0x8c, &t10, 4);   // candidate table[10] slot
        // Identify the 32-bit linker's own .init_array page (ASLR-proof, content-based):
        // table[10] is either 0x000881ed (unrelocated) or base+0x881ed (relocated: low
        // 12 bits 0x1ed, sitting in the 0xf7xxxxxx linker range).
        bool linker_pg = (t10 == 0x000881edu) ||
                         (t10 >= 0xf7000000u && t10 < 0xf8000000u && (t10 & 0xfffu) == 0x1edu);
        if (linker_pg) {
            // Track EVERY physical frame this VA's linker page has used (COW copies to a
            // new frame). Then read t10 from ALL of them: if t10's relocated value ever
            // lands on a PRIOR (old) frame, the store executed but was misdirected there
            // (Pattern D: stale TLB addend after COW), which the fill-only view can't show.
            static uint64_t frames[16]; static int nframes = 0;
            bool seen = false;
            for (int i = 0; i < nframes; ++i) if (frames[i] == pa) { seen = true; break; }
            if (!seen && nframes < 16) frames[nframes++] = pa;
            std::string fr;
            for (int i = 0; i < nframes; ++i) {
                uint32_t v = 0; uc_mem_read(self->uc_, frames[i] + 0x8c, &v, 4);
                char b[48]; std::snprintf(b, sizeof(b), " [%llx]=%08x", (unsigned long long)frames[i], (unsigned)v);
                fr += b;
            }
            uint32_t t9=0,t11=0;
            uc_mem_read(self->uc_, pa+0x88, &t9, 4);
            uc_mem_read(self->uc_, pa+0x90, &t11, 4);
            HW_WARN("cpu.uc", "RELRPROBE va={:#x} type={} pa={:#x} perms={:#x} t9={:#x} t11={:#x} t10-all-frames:{}",
                    (unsigned long long)vaddr, type, (unsigned long long)pa, self->xlat_perms_,
                    (unsigned)t9, (unsigned)t11, fr);
        }
    }
    return true;
}

void UnicornCpu::disasm_line(uint64_t pc) {
    std::printf("  \x1b[2m[cpu]\x1b[0m %#010llx: %s\n", (unsigned long long)pc, disasm_str(pc).c_str());
}

} // namespace hw::cpu
