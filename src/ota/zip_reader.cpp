#include "ota/zip_reader.h"
#include "common/log.h"
#include "common/compress.h"
#include <fstream>
#include <stdexcept>
#include <format>

namespace hw::ota {

namespace {
constexpr uint32_t kEOCD  = 0x06054b50; // End Of Central Directory
constexpr uint32_t kCDFH  = 0x02014b50; // Central Directory File Header
constexpr uint32_t kLFH   = 0x04034b50; // Local File Header
constexpr uint32_t kZ64EOCD = 0x06064b50; // ZIP64 EOCD

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v;
}
} // namespace

ZipReader::ZipReader(std::string path) : path_(std::move(path)) {
    read_central_directory();
}

std::optional<ZipEntry> ZipReader::find(const std::string& name) const {
    for (const auto& e : entries_) if (e.name == name) return e;
    return std::nullopt;
}

Bytes ZipReader::read(const ZipEntry& e) const {
    std::ifstream f(path_, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open zip: " + path_);
    Bytes comp(e.comp_size);
    f.seekg((std::streamoff)e.data_offset);
    f.read(reinterpret_cast<char*>(comp.data()), (std::streamsize)e.comp_size);
    if (!f) throw std::runtime_error("short read on entry " + e.name);
    if (e.method == 0) return comp;
    if (e.method == 8) return compress::inflate_raw(comp);
    throw std::runtime_error(std::format("unsupported zip method {} for {}", e.method, e.name));
}

void ZipReader::read_central_directory() {
    std::ifstream f(path_, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open zip: " + path_);
    f.seekg(0, std::ios::end);
    const uint64_t file_size = (uint64_t)f.tellg();

    // Scan the tail for the EOCD signature (comment can be up to 64KB).
    const uint64_t tail = std::min<uint64_t>(file_size, 66000);
    std::vector<uint8_t> buf(tail);
    f.seekg((std::streamoff)(file_size - tail));
    f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)tail);

    int64_t eocd = -1;
    for (int64_t i = (int64_t)tail - 22; i >= 0; --i) {
        if (rd32(&buf[i]) == kEOCD) { eocd = i; break; }
    }
    if (eocd < 0) throw std::runtime_error("EOCD not found (not a zip?)");

    uint32_t cd_count = rd16(&buf[eocd + 10]);
    uint64_t cd_size  = rd32(&buf[eocd + 12]);
    uint64_t cd_off   = rd32(&buf[eocd + 16]);

    // ZIP64 fallback if any field is saturated.
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || cd_count == 0xFFFFu) {
        for (int64_t i = eocd - 20; i >= 0; --i) {
            if (rd32(&buf[i]) == 0x07064b50) { // ZIP64 EOCD locator
                uint64_t z64off = rd64(&buf[i + 8]);
                std::vector<uint8_t> z(56);
                f.seekg((std::streamoff)z64off);
                f.read(reinterpret_cast<char*>(z.data()), 56);
                if (rd32(z.data()) != kZ64EOCD) throw std::runtime_error("bad ZIP64 EOCD");
                cd_count = (uint32_t)rd64(&z[32]);
                cd_size  = rd64(&z[40]);
                cd_off   = rd64(&z[48]);
                break;
            }
        }
    }

    std::vector<uint8_t> cd(cd_size);
    f.seekg((std::streamoff)cd_off);
    f.read(reinterpret_cast<char*>(cd.data()), (std::streamsize)cd_size);

    size_t p = 0;
    for (uint32_t i = 0; i < cd_count && p + 46 <= cd.size(); ++i) {
        if (rd32(&cd[p]) != kCDFH) throw std::runtime_error("bad central dir header");
        ZipEntry e;
        e.method     = rd16(&cd[p + 10]);
        e.comp_size  = rd32(&cd[p + 20]);
        e.uncomp_size= rd32(&cd[p + 24]);
        uint16_t nlen = rd16(&cd[p + 28]);
        uint16_t elen = rd16(&cd[p + 30]);
        uint16_t clen = rd16(&cd[p + 32]);
        e.local_header_off = rd32(&cd[p + 42]);
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);

        // Parse ZIP64 extra for saturated fields.
        size_t ex = p + 46 + nlen;
        size_t ex_end = ex + elen;
        while (ex + 4 <= ex_end) {
            uint16_t tag = rd16(&cd[ex]);
            uint16_t sz  = rd16(&cd[ex + 2]);
            size_t q = ex + 4;
            if (tag == 0x0001) {
                if (e.uncomp_size == 0xFFFFFFFFu && q + 8 <= ex_end) { e.uncomp_size = rd64(&cd[q]); q += 8; }
                if (e.comp_size   == 0xFFFFFFFFu && q + 8 <= ex_end) { e.comp_size   = rd64(&cd[q]); q += 8; }
                if (e.local_header_off == 0xFFFFFFFFu && q + 8 <= ex_end) { e.local_header_off = rd64(&cd[q]); q += 8; }
            }
            ex += 4 + sz;
        }
        p += 46 + nlen + elen + clen;
        entries_.push_back(std::move(e));
    }

    // Resolve each entry's absolute data offset from its local header.
    for (auto& e : entries_) {
        uint8_t lh[30];
        f.seekg((std::streamoff)e.local_header_off);
        f.read(reinterpret_cast<char*>(lh), 30);
        if (rd32(lh) != kLFH) throw std::runtime_error("bad local header for " + e.name);
        uint16_t nlen = rd16(&lh[26]);
        uint16_t elen = rd16(&lh[28]);
        e.data_offset = e.local_header_off + 30 + nlen + elen;
    }

    HW_DEBUG("ota.zip", "{}: {} entries, CD at {} ({} bytes)", path_, entries_.size(), cd_off, cd_size);
}

} // namespace hw::ota
