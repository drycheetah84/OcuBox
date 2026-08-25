#include "boot/dtb.h"
#include "common/log.h"
#include <cstring>
#include <stdexcept>
#include <format>

namespace hw::boot {

namespace {
constexpr uint32_t FDT_MAGIC       = 0xd00dfeed;
constexpr uint32_t FDT_BEGIN_NODE  = 1;
constexpr uint32_t FDT_END_NODE    = 2;
constexpr uint32_t FDT_PROP        = 3;
constexpr uint32_t FDT_NOP         = 4;
constexpr uint32_t FDT_END         = 9;

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
} // namespace

uint32_t FdtProp::u32(size_t index) const {
    size_t off = index * 4;
    if (off + 4 > data.size()) return 0;
    return be32(data.data() + off);
}
uint64_t FdtProp::u64(size_t index) const {
    return ((uint64_t)u32(index * 2) << 32) | u32(index * 2 + 1);
}
std::string FdtProp::str() const {
    size_t n = 0; while (n < data.size() && data[n]) ++n;
    return std::string(reinterpret_cast<const char*>(data.data()), n);
}
std::vector<std::string> FdtProp::strlist() const {
    std::vector<std::string> out; size_t i = 0;
    while (i < data.size()) {
        size_t n = i; while (n < data.size() && data[n]) ++n;
        if (n > i) out.emplace_back(reinterpret_cast<const char*>(data.data() + i), n - i);
        i = n + 1;
    }
    return out;
}

const FdtProp* FdtNode::prop(const std::string& n) const {
    for (const auto& p : props) if (p.name == n) return &p;
    return nullptr;
}
uint32_t FdtNode::address_cells() const {
    if (parent) { if (auto* p = parent->prop("#address-cells")) return p->u32(); }
    return 2;
}
uint32_t FdtNode::size_cells() const {
    if (parent) { if (auto* p = parent->prop("#size-cells")) return p->u32(); }
    return 1;
}
std::vector<std::pair<uint64_t, uint64_t>> FdtNode::reg() const {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    const FdtProp* r = prop("reg");
    if (!r) return out;
    uint32_t ac = address_cells(), sc = size_cells();
    size_t stride = (ac + sc); // in cells
    size_t total_cells = r->data.size() / 4;
    for (size_t base = 0; base + stride <= total_cells; base += stride) {
        uint64_t addr = 0, size = 0;
        for (uint32_t i = 0; i < ac; ++i) addr = (addr << 32) | r->u32(base + i);
        for (uint32_t i = 0; i < sc; ++i) size = (size << 32) | r->u32(base + ac + i);
        out.emplace_back(addr, size);
    }
    return out;
}
std::string FdtNode::compatible() const {
    if (auto* c = prop("compatible")) return c->str();
    return {};
}
bool FdtNode::compatible_has(const std::string& s) const {
    if (auto* c = prop("compatible")) for (auto& v : c->strlist()) if (v.find(s) != std::string::npos) return true;
    return false;
}

Fdt Fdt::parse(std::span<const uint8_t> blob) {
    if (blob.size() < 40 || be32(blob.data()) != FDT_MAGIC)
        throw std::runtime_error("not an FDT/dtb (bad magic)");

    uint32_t off_struct  = be32(blob.data() + 8);
    uint32_t off_strings = be32(blob.data() + 12);
    Fdt fdt;
    fdt.boot_cpuid_ = be32(blob.data() + 28);

    const uint8_t* s = blob.data() + off_struct;
    const uint8_t* strings = blob.data() + off_strings;
    const uint8_t* end = blob.data() + blob.size();

    uint32_t pos = 0;
    FdtNode* cur = nullptr;
    std::unique_ptr<FdtNode> root;

    auto rd = [&](uint32_t p) -> uint32_t { return be32(s + p); };

    while (s + pos + 4 <= end) {
        uint32_t token = rd(pos); pos += 4;
        if (token == FDT_BEGIN_NODE) {
            const char* name = reinterpret_cast<const char*>(s + pos);
            size_t nlen = std::strlen(name);
            pos += align4((uint32_t)nlen + 1);
            auto node = std::make_unique<FdtNode>();
            node->name = name;
            node->parent = cur;
            FdtNode* raw = node.get();
            if (!cur) { root = std::move(node); }
            else { cur->children.push_back(std::move(node)); }
            cur = raw;
        } else if (token == FDT_END_NODE) {
            if (cur) cur = cur->parent;
        } else if (token == FDT_PROP) {
            uint32_t len = rd(pos); pos += 4;
            uint32_t nameoff = rd(pos); pos += 4;
            FdtProp prop;
            prop.name = reinterpret_cast<const char*>(strings + nameoff);
            prop.data.assign(s + pos, s + pos + len);
            prop.blob_offset = off_struct + pos;   // absolute offset of the value in the blob
            pos += align4(len);
            if (cur) cur->props.push_back(std::move(prop));
        } else if (token == FDT_NOP) {
            // nothing
        } else if (token == FDT_END) {
            break;
        } else {
            throw std::runtime_error(std::format("bad FDT token {:#x} at {}", token, pos - 4));
        }
    }

    fdt.root_ = std::move(root);
    if (!fdt.root_) throw std::runtime_error("empty FDT");
    return fdt;
}

const FdtNode* Fdt::find(const std::string& path) const {
    if (path.empty() || path[0] != '/') return nullptr;
    const FdtNode* n = root_.get();
    if (path == "/") return n;
    size_t i = 1;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        std::string comp = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        const FdtNode* next = nullptr;
        for (auto& c : n->children) if (c->name == comp) { next = c.get(); break; }
        if (!next) return nullptr;
        n = next;
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return n;
}

std::string fdt_node_path(const FdtNode* n) {
    if (!n) return {};
    if (!n->parent) return "/";
    std::vector<const FdtNode*> chain;
    for (const FdtNode* c = n; c && c->parent; c = c->parent) chain.push_back(c);
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) { path += '/'; path += (*it)->name; }
    return path;
}

Bytes fdt_add_prop(std::span<const uint8_t> blob, const std::string& id,
                   const std::string& pname, std::span<const uint8_t> value,
                   bool& modified, std::string& out_path) {
    modified = false;
    Bytes out(blob.begin(), blob.end());          // default: unchanged copy
    Fdt fdt;
    try { fdt = Fdt::parse(blob); } catch (...) { return out; }
    const FdtNode* node = (!id.empty() && id[0] == '/') ? fdt.find(id) : fdt.find_compatible(id);
    if (!node || node->props.empty()) return out;  // need a property to anchor the insert
    out_path = fdt_node_path(node);

    uint32_t totalsize   = be32(blob.data() + 4);
    uint32_t off_strings = be32(blob.data() + 12);
    uint32_t size_strings= be32(blob.data() + 32);
    uint32_t size_struct = be32(blob.data() + 36);

    size_t ins = node->props.front().blob_offset - 12;   // start of the first FDT_PROP token

    // Locate pname in the strings block, or append it (with its NUL).
    const char* strings = reinterpret_cast<const char*>(blob.data()) + off_strings;
    const uint32_t nsz = (uint32_t)pname.size() + 1;
    long nameoff = -1;
    for (uint32_t o = 0; o + nsz <= size_strings; ++o)
        if (std::strcmp(strings + o, pname.c_str()) == 0) { nameoff = o; break; }
    const bool append_name = (nameoff < 0);
    if (append_name) nameoff = size_strings;

    // Property token: FDT_PROP | len | nameoff | value (padded to 4).
    const uint32_t vlen = (uint32_t)value.size();
    const uint32_t vpad = align4(vlen);
    Bytes prop(12 + vpad, 0);
    auto p32 = [&](int o, uint32_t v){ prop[o]=(uint8_t)(v>>24); prop[o+1]=(uint8_t)(v>>16);
                                       prop[o+2]=(uint8_t)(v>>8); prop[o+3]=(uint8_t)v; };
    p32(0, FDT_PROP); p32(4, vlen); p32(8, (uint32_t)nameoff);
    if (vlen) std::memcpy(prop.data() + 12, value.data(), vlen);
    const uint32_t grow = (uint32_t)prop.size();

    // The strings block content is [off_strings, off_strings+size_strings); a new
    // name must be appended right after the content (nameoff = old size_strings),
    // NOT after any trailing padding that may follow it in the blob.
    const size_t strings_end = (size_t)off_strings + size_strings;
    Bytes nb;
    nb.reserve(blob.size() + grow + (append_name ? nsz : 0));
    nb.insert(nb.end(), blob.begin(), blob.begin() + ins);               // up to insert point
    nb.insert(nb.end(), prop.begin(), prop.end());                       // the new property
    nb.insert(nb.end(), blob.begin() + ins, blob.begin() + off_strings); // rest of struct block
    nb.insert(nb.end(), blob.begin() + off_strings, blob.begin() + strings_end); // strings content
    if (append_name) { nb.insert(nb.end(), pname.begin(), pname.end()); nb.push_back(0); }
    nb.insert(nb.end(), blob.begin() + strings_end, blob.end());         // trailing padding, if any

    auto wr32 = [&](size_t o, uint32_t v){ nb[o]=(uint8_t)(v>>24); nb[o+1]=(uint8_t)(v>>16);
                                           nb[o+2]=(uint8_t)(v>>8); nb[o+3]=(uint8_t)v; };
    wr32(4,  totalsize + grow + (append_name ? nsz : 0));  // totalsize
    wr32(12, off_strings + grow);                          // off_dt_strings
    wr32(32, size_strings + (append_name ? nsz : 0));      // size_dt_strings
    wr32(36, size_struct + grow);                          // size_dt_struct
    modified = true;
    return nb;
}

Bytes fdt_disable(std::span<const uint8_t> blob, const std::string& id,
                  bool& modified, std::string& out_path) {
    static const uint8_t kDisabled[] = { 'd','i','s','a','b','l','e','d',0 };
    return fdt_add_prop(blob, id, "status", std::span<const uint8_t>(kDisabled, sizeof kDisabled),
                        modified, out_path);
}

const FdtNode* Fdt::find_compatible(const std::string& s) const {
    const FdtNode* found = nullptr;
    auto dfs = [&](const FdtNode* n, auto&& self) -> void {
        if (found) return;
        if (n->compatible_has(s)) { found = n; return; }
        for (auto& c : n->children) self(c.get(), self);
    };
    if (root_) dfs(root_.get(), dfs);
    return found;
}

} // namespace hw::boot
