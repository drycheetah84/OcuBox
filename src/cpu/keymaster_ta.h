// Synthetic QSEE keymaster@4.1 trusted-app emulation (COMPATIBILITY target).
//
// This is NOT a genuine secure world: there is no device root-of-trust, fuse-derived
// secret, RPMB binding, or real attestation. It provides the minimum deterministic,
// emulator-backed keymaster behaviour Android needs to boot (register the HAL, let
// keystore2 generate/use keys, let vold derive the /data metadata-encryption key).
// Keys are emulator-backed and consistent within (and, being deterministic, across)
// a boot; they are never attestable. See keymaster_ta.cpp for the wire protocol.
//
// The guest talks to the TA over QSEE SEND_DATA (owner=48). The HAL's HIDL/CBOR path
// uses command ids (0x2000 | km_cmd); the ABL's simple-struct path uses raw ids and
// is handled separately in unicorn_cpu.cpp (GET_VERSION/SET_VERSION/...).
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace km {

// Handle a CBOR keymaster command. `cmd` is the full command id as sent (e.g. 0x2108);
// `payload`/`len` is the CBOR request AFTER the 4-byte command id. On success, fills
// `rsp` with the CBOR response bytes (to be written to the guest rsp buffer) and returns
// true; returns false to make the SMC report failure. `log` gates a capped diagnostic log.
bool keymaster_ta_handle(uint32_t cmd, const uint8_t* payload, size_t len,
                         std::vector<uint8_t>& rsp, bool log);

} // namespace km
