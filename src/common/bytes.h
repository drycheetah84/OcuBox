// Small endian-aware byte reading helpers over contiguous buffers.
#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace hw {

using Bytes = std::vector<uint8_t>;

// A bounds-checked forward cursor over a byte span.
class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

    size_t pos() const { return pos_; }
    size_t remaining() const { return data_.size() - pos_; }
    bool eof() const { return pos_ >= data_.size(); }
    void seek(size_t p) { if (p > data_.size()) throw std::out_of_range("seek"); pos_ = p; }
    void skip(size_t n) { need(n); pos_ += n; }

    uint8_t u8() { need(1); return data_[pos_++]; }

    uint32_t u32le() { return read_le<uint32_t>(4); }
    uint64_t u64le() { return read_le<uint64_t>(8); }
    uint32_t u32be() { return read_be<uint32_t>(4); }
    uint64_t u64be() { return read_be<uint64_t>(8); }

    std::span<const uint8_t> take(size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

private:
    void need(size_t n) const {
        if (pos_ + n > data_.size()) throw std::out_of_range("ByteReader overrun");
    }
    template <class T> T read_le(size_t n) {
        need(n); T v = 0;
        for (size_t i = 0; i < n; ++i) v |= (T)data_[pos_ + i] << (8 * i);
        pos_ += n; return v;
    }
    template <class T> T read_be(size_t n) {
        need(n); T v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | data_[pos_ + i];
        pos_ += n; return v;
    }
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

// Read a fixed-length, possibly NUL-padded ASCII field.
inline std::string read_cstr(std::span<const uint8_t> field) {
    size_t n = 0;
    while (n < field.size() && field[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(field.data()), n);
}

} // namespace hw
