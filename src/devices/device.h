// Base interface for a memory-mapped I/O device on the guest bus.
#pragma once
#include <cstdint>

namespace hw::dev {

enum class DevStatus { Ok, Stub, Fail };
inline const char* status_str(DevStatus s) {
    switch (s) { case DevStatus::Ok: return "OK"; case DevStatus::Stub: return "STUB";
                 default: return "FAIL"; }
}

class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual const char* name() const = 0;
    virtual uint64_t base() const = 0;
    virtual uint64_t size() const = 0;
    virtual DevStatus status() const { return DevStatus::Stub; }

    // `offset` is relative to base(); `size` is the access width in bytes.
    virtual uint64_t read(uint64_t offset, unsigned size) = 0;
    virtual void write(uint64_t offset, uint64_t value, unsigned size) = 0;
};

} // namespace hw::dev
