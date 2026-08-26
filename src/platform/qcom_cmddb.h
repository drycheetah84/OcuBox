// Qualcomm Command DB (cmd-db) blob synthesis.
//
// On real Quest 2 hardware XBL/SBL populates a "Command DB" in the reserved
// region at 0x80860000: a table mapping RPMh resource names (clocks, ARC/VRM
// regulators) to their RPMh addresses. Linux's cmd-db driver, rpmh-rsc, clk-rpmh
// and the rpmh-regulators all require it -- without it the WHOLE clock/regulator
// tree (and therefore UFS storage) defers forever ("Invalid Command DB Magic").
//
// We synthesize a minimal valid blob with the STANDALONE bit set, which makes the
// kernel's rpmh vote paths no-ops (no TCS command traffic needed), and containing
// exactly the ARC/VRM resources the UFS clock/regulator bring-up looks up.
#pragma once
#include "common/bytes.h"
#include <cstdint>

namespace hw::platform {

// Physical base + size of the cmd-db reserved region (kona.dtsi reserved-memory).
constexpr uint64_t kCmdDbBase = 0x80860000ull;
constexpr uint64_t kCmdDbSize = 0x20000ull;

// Build a minimal, STANDALONE cmd-db blob (see qcom_cmddb.cpp for the layout).
Bytes build_cmd_db();

} // namespace hw::platform
