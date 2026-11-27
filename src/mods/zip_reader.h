// A small read-only zip reader (stored and deflate entries, no encryption, no
// zip64) on top of zlib, which bbhost already links. Used by the mod loader
// to read a mod that is a .zip without unpacking it first.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mods {

struct ZipEntry {
    std::string name;            // as stored, '\\' not yet turned into '/'
    std::string name_utf8;       // decoded: UTF-8 flag honoured, else the Korean code page (cp949) on Windows
    std::uint16_t flags = 0;
    std::uint16_t method = 0;    // 0 stored, 8 deflate
    std::uint32_t crc = 0;
    std::uint64_t csize = 0, usize = 0, local = 0;
    bool is_dir = false;
};

class ZipReader {
public:
    // False with `error` set when the file is not a readable zip.
    bool open(const std::string& utf8_path, std::string* error);
    const std::vector<ZipEntry>& entries() const { return entries_; }
    // The entry's bytes, checked against its CRC. False with `error` set.
    bool read(const ZipEntry& e, std::vector<std::uint8_t>* out, std::string* error) const;

private:
    std::string path_;
    std::vector<ZipEntry> entries_;
};

// A zip entry's name as text: UTF-8 when flag bit 11 says so, else the bytes
// read as cp949 (Korean Windows archivers) where the platform can, else as is.
std::string zip_decode_name(const std::string& raw, std::uint16_t flags);

}  // namespace mods
