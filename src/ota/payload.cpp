#include "ota/payload.h"
#include "common/compress.h"
#include "common/log.h"
#include <fstream>
#include <stdexcept>
#include <format>

namespace hw::ota {

const char* op_type_name(OpType t) {
    switch (t) {
        case OpType::Replace: return "REPLACE";
        case OpType::ReplaceBz: return "REPLACE_BZ";
        case OpType::ReplaceXz: return "REPLACE_XZ";
        case OpType::Zero: return "ZERO";
        case OpType::Discard: return "DISCARD";
        case OpType::SourceCopy: return "SOURCE_COPY";
        default: return "OTHER";
    }
}

const Partition* Payload::find(const std::string& name) const {
    for (const auto& p : partitions) if (p.name == name) return &p;
    return nullptr;
}

// ---- Minimal protobuf wire-format reader ----
namespace {
class PbReader {
public:
    explicit PbReader(std::span<const uint8_t> d) : d_(d) {}
    bool eof() const { return p_ >= d_.size(); }
    uint64_t varint() {
        uint64_t r = 0; int shift = 0;
        while (true) {
            if (p_ >= d_.size()) throw std::runtime_error("varint overrun");
            uint8_t b = d_[p_++];
            r |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        return r;
    }
    struct Tag { uint32_t field; uint32_t wire; };
    Tag tag() { uint64_t v = varint(); return { (uint32_t)(v >> 3), (uint32_t)(v & 7) }; }
    std::span<const uint8_t> len_bytes() {
        uint64_t n = varint();
        if (p_ + n > d_.size()) throw std::runtime_error("len overrun");
        auto s = d_.subspan(p_, n); p_ += n; return s;
    }
    void skip(uint32_t wire) {
        switch (wire) {
            case 0: varint(); break;
            case 1: p_ += 8; break;
            case 2: { uint64_t n = varint(); p_ += (size_t)n; break; }
            case 5: p_ += 4; break;
            default: throw std::runtime_error("bad wire type");
        }
        if (p_ > d_.size()) throw std::runtime_error("skip overrun");
    }
private:
    std::span<const uint8_t> d_;
    size_t p_ = 0;
};

Extent parse_extent(std::span<const uint8_t> b) {
    PbReader r(b); Extent e;
    while (!r.eof()) { auto t = r.tag();
        if (t.field == 1 && t.wire == 0) e.start_block = r.varint();
        else if (t.field == 2 && t.wire == 0) e.num_blocks = r.varint();
        else r.skip(t.wire);
    }
    return e;
}

InstallOp parse_op(std::span<const uint8_t> b) {
    PbReader r(b); InstallOp op;
    while (!r.eof()) { auto t = r.tag();
        if (t.field == 1 && t.wire == 0) op.type = (OpType)r.varint();
        else if (t.field == 2 && t.wire == 0) op.data_offset = r.varint();
        else if (t.field == 3 && t.wire == 0) op.data_length = r.varint();
        else if (t.field == 6 && t.wire == 2) op.dst_extents.push_back(parse_extent(r.len_bytes()));
        else r.skip(t.wire);
    }
    return op;
}

DynamicPartitionGroup parse_dpgroup(std::span<const uint8_t> b) {
    PbReader r(b); DynamicPartitionGroup g;
    while (!r.eof()) { auto t = r.tag();
        if (t.field == 1 && t.wire == 2) { auto s = r.len_bytes(); g.name.assign((const char*)s.data(), s.size()); }
        else if (t.field == 2 && t.wire == 0) g.maximum_size = r.varint();
        else if (t.field == 3 && t.wire == 2) { auto s = r.len_bytes(); g.partition_names.emplace_back((const char*)s.data(), s.size()); }
        else r.skip(t.wire);
    }
    return g;
}

DynamicPartitionMetadata parse_dap(std::span<const uint8_t> b) {
    PbReader r(b); DynamicPartitionMetadata m; m.present = true;
    while (!r.eof()) { auto t = r.tag();
        if (t.field == 1 && t.wire == 2) m.groups.push_back(parse_dpgroup(r.len_bytes()));
        else r.skip(t.wire);
    }
    return m;
}

Partition parse_partition(std::span<const uint8_t> b) {
    PbReader r(b); Partition p;
    while (!r.eof()) { auto t = r.tag();
        if (t.field == 1 && t.wire == 2) { auto s = r.len_bytes(); p.name.assign((const char*)s.data(), s.size()); }
        else if (t.field == 7 && t.wire == 2) { // PartitionInfo new_partition_info
            PbReader pi(r.len_bytes());
            while (!pi.eof()) { auto it = pi.tag();
                if (it.field == 1 && it.wire == 0) p.size = pi.varint();
                else if (it.field == 2 && it.wire == 2) { auto h = pi.len_bytes(); p.sha256.assign(h.begin(), h.end()); }
                else pi.skip(it.wire);
            }
        }
        else if (t.field == 8 && t.wire == 2) p.ops.push_back(parse_op(r.len_bytes()));
        else r.skip(t.wire);
    }
    return p;
}
} // namespace

Payload parse_payload(const ZipReader& zip) {
    auto pe = zip.find("payload.bin");
    if (!pe) throw std::runtime_error("payload.bin not found in OTA");
    if (pe->method != 0)
        throw std::runtime_error("payload.bin is compressed inside the zip (unexpected)");

    std::ifstream f(zip.path(), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open OTA zip for reading payload");

    uint8_t head[24];
    f.seekg((std::streamoff)pe->data_offset);
    f.read(reinterpret_cast<char*>(head), 24);
    if (std::string_view((const char*)head, 4) != "CrAU")
        throw std::runtime_error("bad payload magic");

    Payload pl;
    pl.payload_offset_in_zip = pe->data_offset;
    ByteReader hr(std::span<const uint8_t>(head, 24));
    hr.skip(4);
    pl.version = hr.u64be();
    pl.manifest_size = hr.u64be();
    pl.metadata_sig_size = hr.u32be();
    pl.header_size = 24;

    Bytes manifest(pl.manifest_size);
    f.seekg((std::streamoff)(pe->data_offset + pl.header_size));
    f.read(reinterpret_cast<char*>(manifest.data()), (std::streamsize)pl.manifest_size);

    PbReader r(manifest);
    while (!r.eof()) { auto t = r.tag();
        if (t.field == 3 && t.wire == 0) pl.block_size = (uint32_t)r.varint();
        else if (t.field == 13 && t.wire == 2) pl.partitions.push_back(parse_partition(r.len_bytes()));
        else if (t.field == 15 && t.wire == 2) pl.dap = parse_dap(r.len_bytes());  // dynamic_partition_metadata
        else r.skip(t.wire);
    }
    pl.data_blob_start = pl.header_size + pl.manifest_size + pl.metadata_sig_size;

    HW_INFO("ota.payload", "payload v{} block={} manifest={}B parts={}",
            pl.version, pl.block_size, pl.manifest_size, pl.partitions.size());
    return pl;
}

Bytes extract_partition(const ZipReader& zip, const Payload& pl, const std::string& name) {
    const Partition* part = pl.find(name);
    if (!part) throw std::runtime_error("partition not found: " + name);

    std::ifstream f(zip.path(), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open OTA zip for extraction");

    Bytes out(part->size, 0);
    const uint64_t blob_base = pl.payload_offset_in_zip + pl.data_blob_start;
    Bytes blob;

    for (size_t i = 0; i < part->ops.size(); ++i) {
        const InstallOp& op = part->ops[i];
        if (op.dst_extents.empty()) continue;
        const uint64_t dst_off = op.dst_extents[0].start_block * pl.block_size;
        uint64_t dst_len = 0;
        for (const auto& e : op.dst_extents) dst_len += e.num_blocks * pl.block_size;
        if (dst_off + dst_len > out.size())
            throw std::runtime_error(std::format("op {} dst out of range in {}", i, name));

        if (op.type == OpType::Zero || op.type == OpType::Discard) {
            std::fill(out.begin() + dst_off, out.begin() + dst_off + dst_len, (uint8_t)0);
            continue;
        }

        blob.resize(op.data_length);
        f.seekg((std::streamoff)(blob_base + op.data_offset));
        f.read(reinterpret_cast<char*>(blob.data()), (std::streamsize)op.data_length);
        if (!f) throw std::runtime_error(std::format("short read on op {} of {}", i, name));

        switch (op.type) {
            case OpType::Replace:
                if (op.data_length > dst_len) throw std::runtime_error("REPLACE too big");
                std::copy(blob.begin(), blob.end(), out.begin() + dst_off);
                break;
            case OpType::ReplaceXz: {
                Bytes d = compress::xz_decode(blob, (size_t)dst_len);
                std::copy(d.begin(), d.end(), out.begin() + dst_off);
                break;
            }
            case OpType::ReplaceBz: {
                Bytes d = compress::bz2_decode(blob, (size_t)dst_len);
                std::copy(d.begin(), d.end(), out.begin() + dst_off);
                break;
            }
            default:
                throw std::runtime_error(std::format(
                    "unsupported op type {} in {} (full-OTA only)", op_type_name(op.type), name));
        }
    }

    HW_INFO("ota.payload", "extracted '{}' -> {} bytes ({} ops)", name, out.size(), part->ops.size());
    return out;
}

} // namespace hw::ota
