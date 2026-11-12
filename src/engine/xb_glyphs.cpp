#include "engine/xb_glyphs.h"
#include <mutex>

#include "hle/fs.h"
#include "log.h"

#include <zlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Bumped whenever the way the file is made changes, so installs remake it.
constexpr const char* kStamp = "xb-glyphs 2";
constexpr std::size_t kEntry = 36;       // a PS4 TPF entry header
constexpr std::size_t kDcxHeader = 0x4c;  // DCX + DFLT header, up to the zlib stream

struct XbTexture {
    const char* name;
    const unsigned char* hdr;
    const unsigned char* data;
    std::size_t size;
};
#include "engine/xb_glyphs.inc"

using Bytes = std::vector<std::uint8_t>;

struct Entry {
    unsigned char hdr[kEntry];
    std::string name;  // ASCII; the file keeps it as UTF-16
    Bytes data;
};

bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
bool write_file(const fs::path& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint32_t be32(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void put_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255;
}
void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = (v >> 24) & 255; p[1] = (v >> 16) & 255; p[2] = (v >> 8) & 255; p[3] = v & 255;
}

// The layout make_xb_tpf.py expects: DCX, DFLT, "DCA\0" at 0x44, a zlib stream at 0x4c.
bool dcx_inflate(const Bytes& d, Bytes& raw, std::string& why) {
    if (d.size() < kDcxHeader + 6 || std::memcmp(d.data(), "DCX\0", 4) != 0 || std::memcmp(d.data() + 0x28, "DFLT", 4) != 0 ||
        std::memcmp(d.data() + 0x44, "DCA\0", 4) != 0 || d[0x4c] != 0x78 || d[0x4d] != 0xda) {
        why = "not a DCX DFLT file of the expected layout";
        return false;
    }
    const std::uint32_t n = be32(d.data() + 0x1c);
    if (n < 16 || n > (256u << 20)) {
        why = "unexpected size";
        return false;
    }
    raw.assign(n, 0);
    uLongf got = n;
    if (uncompress(raw.data(), &got, d.data() + kDcxHeader, static_cast<uLong>(d.size() - kDcxHeader)) != Z_OK || got != n) {
        why = "cannot be inflated";
        return false;
    }
    return true;
}

// PS4 layout, UTF-16 names, no float structs: the only kind make_xb_tpf.py handles.
bool parse_tpf(const Bytes& raw, std::uint8_t meta[4], std::vector<Entry>& ents, std::string& why) {
    if (raw.size() < 16 || std::memcmp(raw.data(), "TPF\0", 4) != 0) {
        why = "not a TPF";
        return false;
    }
    const std::uint32_t fc = le32(raw.data() + 8);
    std::memcpy(meta, raw.data() + 12, 4);
    if (meta[0] != 4 || meta[2] != 1) {
        why = "not PS4 / UTF-16";
        return false;
    }
    if (fc == 0 || fc > 4096 || 16 + static_cast<std::size_t>(fc) * kEntry > raw.size()) {
        why = "bad texture count";
        return false;
    }
    for (std::uint32_t i = 0; i < fc; ++i) {
        const std::uint8_t* h = raw.data() + 16 + i * kEntry;
        const std::uint32_t off = le32(h), size = le32(h + 4), no = le32(h + 24), hasfloat = le32(h + 28);
        if (hasfloat != 0 || static_cast<std::size_t>(off) + size > raw.size() || no >= raw.size()) {
            why = "an entry is not of the expected kind";
            return false;
        }
        std::string name;
        std::size_t e = no;
        for (;; e += 2) {
            if (e + 2 > raw.size()) {
                why = "a name runs off the file";
                return false;
            }
            if (raw[e] == 0 && raw[e + 1] == 0) break;
            name.push_back(raw[e + 1] == 0 ? static_cast<char>(raw[e]) : '?');
        }
        Entry en;
        std::memcpy(en.hdr, h, kEntry);
        en.name = name;
        en.data.assign(raw.begin() + off, raw.begin() + off + size);
        ents.push_back(std::move(en));
    }
    return true;
}

Bytes build_tpf(const std::uint8_t meta[4], std::vector<Entry>& ents) {
    const std::size_t n = ents.size();
    std::size_t pos = 16 + n * kEntry;
    Bytes names;
    std::vector<std::uint32_t> nameoff(n);
    for (std::size_t i = 0; i < n; ++i) {
        nameoff[i] = static_cast<std::uint32_t>(pos + names.size());
        for (char c : ents[i].name) {
            names.push_back(static_cast<std::uint8_t>(c));
            names.push_back(0);
        }
        names.push_back(0);
        names.push_back(0);
    }
    // The game's own file starts the texture data on a 16-byte boundary (names padded up to it); PS4 textures want that.
    const std::size_t data_start = (pos + names.size() + 15) & ~static_cast<std::size_t>(15);
    std::size_t data_len = 0;
    for (const Entry& e : ents) data_len += e.data.size();
    Bytes out(data_start + data_len, 0);
    std::memcpy(out.data(), "TPF\0", 4);
    put_le32(out.data() + 4, static_cast<std::uint32_t>(data_len));
    put_le32(out.data() + 8, static_cast<std::uint32_t>(n));
    std::memcpy(out.data() + 12, meta, 4);
    std::size_t at = data_start;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t* h = out.data() + 16 + i * kEntry;
        std::memcpy(h, ents[i].hdr, kEntry);
        put_le32(h, static_cast<std::uint32_t>(at));
        put_le32(h + 24, nameoff[i]);
        if (!ents[i].data.empty()) std::memcpy(out.data() + at, ents[i].data.data(), ents[i].data.size());
        at += ents[i].data.size();
    }
    std::memcpy(out.data() + pos, names.data(), names.size());
    return out;
}

// No new names: the 20 KG_<name> textures the game has get the Xbox pictures instead.
bool make(const Bytes& base, Bytes& result, std::string& why) {
    Bytes raw;
    if (!dcx_inflate(base, raw, why)) return false;
    std::uint8_t meta[4];
    std::vector<Entry> ents;
    if (!parse_tpf(raw, meta, ents, why)) return false;
    for (const XbTexture& t : kXbTextures) {
        const std::string orig(t.name, std::strlen(t.name) - 3);
        Entry* hit = nullptr;
        for (Entry& e : ents) {
            if (e.name == orig) hit = &e;
        }
        if (!hit) {
            why = orig + " is not in the game's file";
            return false;
        }
        std::memcpy(hit->hdr, t.hdr, kEntry);
        hit->data.assign(t.data, t.data + t.size);
    }
    const Bytes tpf = build_tpf(meta, ents);
    uLongf cap = compressBound(static_cast<uLong>(tpf.size()));
    Bytes comp(cap);
    if (compress2(comp.data(), &cap, tpf.data(), static_cast<uLong>(tpf.size()), 9) != Z_OK) {
        why = "cannot be compressed";
        return false;
    }
    comp.resize(cap);
    result.assign(base.begin(), base.begin() + kDcxHeader);
    put_be32(result.data() + 0x1c, static_cast<std::uint32_t>(tpf.size()));
    put_be32(result.data() + 0x20, static_cast<std::uint32_t>(comp.size()));
    result.insert(result.end(), comp.begin(), comp.end());
    return true;
}

}  // namespace

namespace {
std::mutex g_mu;
std::string g_out;  // the made folder, once it is ready
bool g_xbox = false;
bool g_applied = false;
// The made file goes in only when it is ready and the pad at start is an Xbox one,
// and only once; a PlayStation pad leaves the game's own file as it is.
void try_apply_locked() {
    if (g_applied || g_out.empty() || !g_xbox) return;
    g_applied = true;
    hle_fs_add_generated_root(g_out.c_str());
    host_log("xb glyphs: Xbox pad at start - the menu pictures are the Xbox ones");
}
}  // namespace

void xb_glyphs_install(const char* data) {
    if (!data || !*data) {
        host_log("xb glyphs: off - no data folder");
        return;
    }
    const std::string src = hle_fs_game_file("dvdroot_ps4/menu/common.tpf.dcx");
    Bytes base;
    if (!read_file(src, base)) {
        host_log("xb glyphs: off - cannot read %s; the Xbox pictures stay out", src.c_str());
        return;
    }
    const fs::path out = fs::path(data) / "bbhost" / "xb-assets";
    const std::string stamp = std::string(kStamp) + " " + std::to_string(base.size()) + " inplace";
    Bytes have;
    const bool fresh = read_file(out / "stamp", have) && std::string(have.begin(), have.end()) == stamp &&
                       fs::exists(out / "dvdroot_ps4" / "menu" / "common.tpf.dcx");
    if (!fresh) {
        Bytes made;
        std::string why;
        if (!make(base, made, why)) {
            host_log("xb glyphs: off - %s: %s", src.c_str(), why.c_str());
            return;
        }
        if (!write_file(out / "dvdroot_ps4" / "menu" / "common.tpf.dcx", made) || !write_file(out / "stamp", Bytes(stamp.begin(), stamp.end()))) {
            host_log("xb glyphs: off - cannot write %s", out.string().c_str());
            return;
        }
        host_log("xb glyphs: made menu/common.tpf.dcx (%zu bytes) with %zu Xbox pictures in %s", made.size(),
                 sizeof(kXbTextures) / sizeof(kXbTextures[0]), out.string().c_str());
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_out = out.string();
    try_apply_locked();
}

void xb_glyphs_pad(bool xbox) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_xbox = xbox;
    try_apply_locked();
}
