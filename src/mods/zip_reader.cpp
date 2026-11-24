#include "mods/zip_reader.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mods {
namespace {

namespace fs = std::filesystem;

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99) return false;
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string latin1_to_utf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) {
            o += static_cast<char>(c);
        } else {
            o += static_cast<char>(0xC0 | (c >> 6));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return o;
}

}  // namespace

std::string zip_decode_name(const std::string& raw, std::uint16_t flags) {
    if (flags & 0x800) return raw;
    bool ascii = true;
    for (unsigned char c : raw) ascii = ascii && c < 0x80;
    if (ascii) return raw;
#if defined(_WIN32)
    // Korean Windows archivers store the name in the system code page.
    int n = MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (n > 0) {
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), &w[0], n);
        int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
        if (m > 0) {
            std::string out(static_cast<std::size_t>(m), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], m, nullptr, nullptr);
            return out;
        }
    }
#endif
    // Some archivers write UTF-8 without the flag.
    if (valid_utf8(raw)) return raw;
    return latin1_to_utf8(raw);
}

bool ZipReader::open(const std::string& utf8_path, std::string* error) {
    entries_.clear();
    path_ = utf8_path;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::ifstream f(fs::u8path(utf8_path), std::ios::binary);
    if (!f) return fail("cannot open");
    f.seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(f.tellg());
    if (size < 22) return fail("too small to be a zip");
    // The end-of-central-directory record is in the last 64 KB + 22 bytes.
    const std::uint64_t tail = std::min<std::uint64_t>(size, 22 + 65535);
    std::vector<std::uint8_t> t(static_cast<std::size_t>(tail));
    f.seekg(static_cast<std::streamoff>(size - tail));
    f.read(reinterpret_cast<char*>(t.data()), static_cast<std::streamsize>(t.size()));
    if (!f) return fail("read error");
    std::size_t eocd = std::string::npos;
    for (std::size_t i = t.size() - 22 + 1; i-- > 0;) {
        if (rd32(&t[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return fail("no zip directory (not a zip)");
    const std::uint32_t count = rd16(&t[eocd + 10]);
    const std::uint32_t cd_size = rd32(&t[eocd + 12]);
    const std::uint32_t cd_off = rd32(&t[eocd + 16]);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) return fail("zip64 archives are not supported");
    if (static_cast<std::uint64_t>(cd_off) + cd_size > size) return fail("damaged zip directory");
    std::vector<std::uint8_t> cd(cd_size);
    f.seekg(cd_off);
    f.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd.size()));
    if (!f) return fail("read error");
    std::size_t p = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) return fail("damaged zip directory");
        ZipEntry e;
        e.flags = rd16(&cd[p + 8]);
        e.method = rd16(&cd[p + 10]);
        e.crc = rd32(&cd[p + 16]);
        e.csize = rd32(&cd[p + 20]);
        e.usize = rd32(&cd[p + 24]);
        const std::size_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
        e.local = rd32(&cd[p + 42]);
        if (p + 46 + nl + xl + cl > cd.size()) return fail("damaged zip directory");
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nl);
        e.name_utf8 = zip_decode_name(e.name, e.flags);
        e.is_dir = !e.name.empty() && (e.name.back() == '/' || e.name.back() == '\\');
        entries_.push_back(std::move(e));
        p += 46 + nl + xl + cl;
    }
    return true;
}

bool ZipReader::read(const ZipEntry& e, std::vector<std::uint8_t>* out, std::string* error) const {
    auto fail = [&](const std::string& why) {
        if (error) *error = e.name + ": " + why;
        return false;
    };
    if (e.flags & 1) return fail("encrypted entries are not supported");
    if (e.method != 0 && e.method != 8) return fail("unsupported compression method");
    std::ifstream f(fs::u8path(path_), std::ios::binary);
    if (!f) return fail("cannot open the zip");
    std::uint8_t lh[30];
    f.seekg(static_cast<std::streamoff>(e.local));
    f.read(reinterpret_cast<char*>(lh), sizeof(lh));
    if (!f || rd32(lh) != 0x04034b50) return fail("damaged local header");
    const std::uint64_t data_at = e.local + 30 + rd16(lh + 26) + rd16(lh + 28);
    std::vector<std::uint8_t> comp(static_cast<std::size_t>(e.csize));
    f.seekg(static_cast<std::streamoff>(data_at));
    if (!comp.empty()) f.read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()));
    if (!f) return fail("read error");
    out->assign(static_cast<std::size_t>(e.usize), 0);
    if (e.method == 0) {
        if (e.csize != e.usize) return fail("damaged entry");
        std::memcpy(out->data(), comp.data(), comp.size());
    } else {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail("zlib failed");
        zs.next_in = comp.data();
        zs.avail_in = static_cast<uInt>(comp.size());
        zs.next_out = out->data();
        zs.avail_out = static_cast<uInt>(out->size());
        const int rc = inflate(&zs, Z_FINISH);
        const bool ok = rc == Z_STREAM_END && zs.total_out == out->size();
        inflateEnd(&zs);
        if (!ok) return fail("damaged compressed data");
    }
    if (static_cast<std::uint32_t>(crc32(0, out->data(), static_cast<uInt>(out->size()))) != e.crc) return fail("CRC mismatch");
    return true;
}

}  // namespace mods
