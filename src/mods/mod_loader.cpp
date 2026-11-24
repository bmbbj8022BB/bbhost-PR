#include "mods/mod_loader.h"

#include "mods/anibnd_merge.h"
#include "mods/fs_utf8.h"
#include "mods/param_merge.h"
#include "mods/zip_reader.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace mods {
namespace {

namespace fs = std::filesystem;

fs::path P(const std::string& s) { return path_from_utf8(s); }
std::string U8(const fs::path& p) { return path_to_utf8(p); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t n = s.find(sep, at);
        out.push_back(s.substr(at, n == std::string::npos ? n : n - at));
        if (n == std::string::npos) break;
        at = n + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& v, std::size_t from = 0, std::size_t to = std::string::npos, char sep = '/') {
    std::string o;
    if (to > v.size()) to = v.size();
    for (std::size_t i = from; i < to; ++i) {
        if (i > from) o += sep;
        o += v[i];
    }
    return o;
}

// The absolute, normalized path as text (native separators, no trailing one).
std::string abspath(const std::string& s) {
    std::error_code ec;
    fs::path p = fs::absolute(P(s), ec);
    if (ec) p = P(s);
    p = p.lexically_normal();
    p.make_preferred();
    std::string r = U8(p);
    while (r.size() > 1 && (r.back() == '/' || r.back() == '\\') && !(r.size() == 3 && r[1] == ':')) r.pop_back();
    return r;
}

// os.path.normcase: lower case with backslashes on Windows, as is elsewhere.
std::string normcase(const std::string& s) {
#if defined(_WIN32)
    std::string r = lower(s);
    for (char& c : r)
        if (c == '/') c = '\\';
    return r;
#else
    return s;
#endif
}

bool is_dir(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(P(p), ec);
}
bool is_file(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(P(p), ec);
}
bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(P(p), ec);
}

bool read_file(const std::string& path, Bytes* out, std::string* error) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto n = f.tellg();
    f.seekg(0);
    out->assign(static_cast<std::size_t>(n), 0);
    if (n > 0) f.read(reinterpret_cast<char*>(out->data()), n);
    if (!f) {
        if (error) *error = "read error: " + path;
        return false;
    }
    return true;
}

// Writes next to `dst` and renames over it.
bool write_atomic(const std::string& dst, const Bytes& data, std::string* error) {
    std::error_code ec;
    fs::create_directories(P(dst).parent_path(), ec);
    const std::string tmp = dst + ".bbmod_tmp";
    {
        std::ofstream f(P(tmp), std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + tmp;
            return false;
        }
        if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) {
            if (error) *error = "write error: " + tmp;
            fs::remove(P(tmp), ec);
            return false;
        }
    }
    fs::rename(P(tmp), P(dst), ec);
    if (ec) {
        if (error) *error = "cannot replace " + dst + ": " + ec.message();
        fs::remove(P(tmp), ec);
        return false;
    }
    return true;
}

bool copy_replace(const std::string& from, const std::string& to, std::string* error) {
    Bytes b;
    return read_file(from, &b, error) && write_atomic(to, b, error);
}

// ---- SHA-1: only to name the backup folder the way the Python loader did.
std::string sha1_hex8(const std::string& msg) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = msg;
    const std::uint64_t bits = static_cast<std::uint64_t>(m.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; --i) m += static_cast<char>((bits >> (i * 8)) & 0xFF);
    auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(m[off + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(m[off + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(m[off + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(m[off + i * 4 + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", h[0]);
    return buf;
}

const char* const kIgnoreFiles[] = {".ds_store", "thumbs.db", "desktop.ini"};
bool ignored_file(const std::string& n) {
    const std::string l = lower(n);
    for (const char* x : kIgnoreFiles)
        if (l == x) return true;
    return false;
}
bool ignored_dir(const std::string& n) { return lower(n) == "__macosx"; }

// Extensions silently ignored as readme/preview files that have no original.
bool doc_ext(const std::string& file) {
    static const char* const kExt[] = {".txt", ".md", ".url", ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".nfo", ".pdf",
                                       ".html", ".htm", ".rtf", ".doc", ".docx", ".webp", ".lnk"};
    const std::size_t dot = file.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string e = lower(file.substr(dot));
    for (const char* x : kExt)
        if (e == x) return true;
    return false;
}

// A path piece list that cannot leave the overlay; nullopt-like (false) when unsafe.
bool safe_parts(const std::vector<std::string>& in, std::vector<std::string>* out) {
    out->clear();
    for (const std::string& p : in) {
        if (p == ".") continue;
        bool blank = true;
        for (char c : p) blank = blank && std::isspace(static_cast<unsigned char>(c));
        if (p == ".." || p.find(':') != std::string::npos || blank) return false;
        out->push_back(p);
    }
    return !out->empty();
}

bool rel_safe(const std::string& rel) {
    if (rel.empty()) return false;
    for (const std::string& p : split(rel, '/'))
        if (p.empty() || p == "." || p == ".." || p.find(':') != std::string::npos || p.find('\\') != std::string::npos) return false;
    return true;
}

fs::path join_rel(const std::string& root, const std::string& rel) {
    fs::path p = P(root);
    for (const std::string& c : split(rel, '/')) p /= P(c);
    return p;
}

}  // namespace

// ------------------------------------------------------------------ mods
std::string FileRef::rel() const { return join(parts); }

bool FileRef::read(Bytes* out, std::string* error) const {
    if (zip) return zip->read(zip->entries()[zip_index], out, error);
    return read_file(src, out, error);
}

bool FileRef::write_to(const std::string& dst, std::string* error) const {
    Bytes b;
    return read(&b, error) && write_atomic(dst, b, error);
}

std::vector<FileRef> ModSource::files(std::string* error) const {
    std::vector<FileRef> out;
    if (!is_zip) {
        std::vector<std::pair<std::string, std::vector<std::string>>> stack{{path, {}}};
        while (!stack.empty()) {
            auto [dir, parts] = std::move(stack.back());
            stack.pop_back();
            std::error_code ec;
            for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = U8(it->path().filename());
                std::error_code e2;
                const fs::file_status st = it->symlink_status(e2);
                auto next = parts;
                next.push_back(name);
                if (fs::is_directory(st)) {
                    if (!ignored_dir(name)) stack.push_back({U8(it->path()), next});
                } else if (!ignored_file(name)) {
                    FileRef r;
                    r.parts = next;
                    r.src = U8(it->path());
                    std::error_code e3;
                    r.size = static_cast<std::uint64_t>(fs::file_size(it->path(), e3));
                    const auto mt = fs::last_write_time(it->path(), e3);
                    r.sig = std::to_string(r.size) + ":" + std::to_string(static_cast<long long>(mt.time_since_epoch().count()));
                    out.push_back(std::move(r));
                }
            }
        }
    } else {
        auto z = std::make_shared<ZipReader>();
        std::string why;
        if (!z->open(path, &why)) {
            if (error) *error = name + ": " + why;
            return out;
        }
        const auto& es = z->entries();
        for (std::size_t i = 0; i < es.size(); ++i) {
            if (es[i].is_dir) continue;
            std::string n = es[i].name_utf8;
            for (char& c : n)
                if (c == '\\') c = '/';
            std::vector<std::string> raw;
            for (const std::string& p : split(n, '/'))
                if (!p.empty()) raw.push_back(p);
            FileRef r;
            if (!safe_parts(raw, &r.parts) || ignored_file(r.parts.back())) continue;
            bool skip = false;
            for (const std::string& p : r.parts) skip = skip || ignored_dir(p);
            if (skip) continue;  // an entry with '..' or a drive letter would leave the overlay
            r.src = path;
            r.zip = z;
            r.zip_index = i;
            r.size = es[i].usize;
            char buf[40];
            std::snprintf(buf, sizeof(buf), "z%08x:%llu", es[i].crc, static_cast<unsigned long long>(es[i].usize));
            r.sig = buf;
            out.push_back(std::move(r));
        }
    }
    std::sort(out.begin(), out.end(), [](const FileRef& a, const FileRef& b) { return lower(a.rel()) < lower(b.rel()); });
    return out;
}

namespace {
std::string modlist_path(const std::string& mods_dir) { return U8(P(mods_dir) / ".bbmods" / "modlist.txt"); }
}  // namespace

std::vector<ModSource> list_mods(const std::string& mods_dir) {
    std::vector<ModSource> found;
    std::error_code ec;
    if (!is_dir(mods_dir)) return found;
    for (fs::directory_iterator it(P(mods_dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = U8(it->path().filename());
        if (n.empty() || n[0] == '.' || n[0] == '_') continue;
        std::error_code e2;
        ModSource m;
        m.path = U8(it->path());
        m.name = n;
        if (fs::is_directory(it->path(), e2)) {
            m.is_zip = false;
        } else if (fs::is_regular_file(it->path(), e2) && lower(n).size() > 4 && lower(n).substr(lower(n).size() - 4) == ".zip") {
            m.is_zip = true;
        } else {
            continue;
        }
        found.push_back(std::move(m));
    }
    std::sort(found.begin(), found.end(), [](const ModSource& a, const ModSource& b) { return lower(a.name) < lower(b.name); });
    // The saved order and switches.
    std::vector<ModSource> out;
    std::ifstream f(P(modlist_path(mods_dir)), std::ios::binary);
    std::string line;
    while (f && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != '\t') continue;
        const std::string name = line.substr(2);
        auto it = std::find_if(found.begin(), found.end(), [&](const ModSource& m) { return m.name == name; });
        if (it == found.end()) continue;
        it->enabled = line[0] == '1';
        out.push_back(std::move(*it));
        found.erase(it);
    }
    for (ModSource& m : found) out.push_back(std::move(m));
    return out;
}

bool save_modlist(const std::string& mods_dir, const std::vector<ModSource>& mods, std::string* error) {
    std::string text;
    for (const ModSource& m : mods) text += std::string(m.enabled ? "1" : "0") + "\t" + m.name + "\n";
    Bytes b(text.begin(), text.end());
    return write_atomic(modlist_path(mods_dir), b, error);
}

std::vector<std::string> import_mods(const std::vector<std::string>& paths, const std::string& mods_dir, const LogFn& log) {
    std::vector<std::string> added;
    std::error_code ec;
    fs::create_directories(P(mods_dir), ec);
    const std::string mods_abs = normcase(abspath(mods_dir));
    auto say = [&](const std::string& s) {
        if (log) log(s);
    };
    std::vector<ModSource> current = list_mods(mods_dir);
    for (const std::string& raw : paths) {
        const std::string p = abspath(raw);
        std::string name = U8(P(p).filename());
        if (name.empty()) name = "mod";
        if (normcase(abspath(U8(P(p).parent_path()))) == mods_abs) {
            say("Already inside the mods folder: " + name);
            continue;
        }
        std::string clean = name;
        while (!clean.empty() && (clean[0] == '.' || clean[0] == '_')) clean.erase(0, 1);
        if (clean.empty()) clean = "mod";
        auto unique = [&](const std::string& base, const std::string& ext) {
            fs::path cand = P(mods_dir) / P(base + ext);
            for (int n = 2; fs::exists(cand, ec); ++n) cand = P(mods_dir) / P(base + " (" + std::to_string(n) + ")" + ext);
            return cand;
        };
        try {
            if (is_dir(p)) {
                const std::string np = normcase(p);
                if (np == mods_abs || (mods_abs + std::string(1, fs::path::preferred_separator)).rfind(np + std::string(1, fs::path::preferred_separator), 0) == 0) {
                    say("Cannot import the mods folder itself (or a folder containing it): " + name);
                    continue;
                }
                const fs::path dst = unique(clean, "");
                say("Copying folder: " + name);
                fs::copy(P(p), dst, fs::copy_options::recursive);
                added.push_back(U8(dst.filename()));
            } else if (is_file(p) && lower(name).size() > 4 && lower(name).substr(lower(name).size() - 4) == ".zip") {
                ZipReader z;
                std::string why;
                if (!z.open(p, &why)) {
                    say("Not a valid zip: " + name + " (" + why + ")");
                    continue;
                }
                const std::size_t dot = clean.rfind('.');
                const fs::path dst = unique(clean.substr(0, dot), clean.substr(dot));
                say("Copying zip: " + name);
                fs::copy_file(P(p), dst);
                added.push_back(U8(dst.filename()));
            } else {
                say("Unsupported (only a zip or a folder): " + name);
                continue;
            }
            say("Mod added: " + added.back());
        } catch (const std::exception& ex) {
            say("Import failed (" + name + "): " + ex.what());
        }
    }
    if (!added.empty()) {
        // The new mods go last, on.
        std::vector<ModSource> all = list_mods(mods_dir);
        std::vector<ModSource> ordered;
        for (ModSource& m : all)
            if (std::find(added.begin(), added.end(), m.name) == added.end()) ordered.push_back(m);
        for (ModSource& m : all)
            if (std::find(added.begin(), added.end(), m.name) != added.end()) {
                m.enabled = true;
                ordered.push_back(m);
            }
        std::string why;
        save_modlist(mods_dir, ordered, &why);
    }
    return added;
}

// ------------------------------------------------------------------ the game
std::string find_update_folder(const std::string& app0) {
    const fs::path a = P(abspath(app0));
    const std::string name = U8(a.filename());
    const fs::path parent = a.parent_path();
    for (const char* suf : {"-UPDATE", "-patch"}) {
        std::string s = suf;
        std::string lowers = lower(s), uppers = s;
        for (char& c : uppers) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (const std::string& cand : {name + s, name + lowers, name + uppers}) {
            const fs::path p = parent / P(cand);
            std::error_code ec;
            if (fs::is_directory(p, ec)) return U8(p);
        }
    }
    return "";
}

std::string GameIndex::full_path(const File& f) const { return U8(join_rel(roots[static_cast<std::size_t>(f.root)], f.rel)); }

void GameIndex::build(const std::vector<std::string>& in_roots, const std::vector<std::string>& exclude, const LogFn& progress) {
    roots.clear();
    files.clear();
    by_name.clear();
    dirs_by_name.clear();
    dirs.clear();
    dirnames.clear();
    by_lower.clear();
    for (const std::string& r : in_roots) roots.push_back(abspath(r));
    std::set<std::string> excl;
    for (const std::string& e : exclude)
        if (!e.empty()) excl.insert(normcase(abspath(e)));
    std::size_t n = 0;
    for (std::size_t ri = 0; ri < roots.size(); ++ri) {
        std::vector<std::pair<std::string, std::string>> stack{{roots[ri], ""}};
        while (!stack.empty()) {
            auto [dir, rel] = std::move(stack.back());
            stack.pop_back();
            std::error_code ec;
            for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = U8(it->path().filename());
                const std::string r = rel.empty() ? name : rel + "/" + name;
                std::error_code e2;
                if (it->is_directory(e2)) {
                    if (excl.count(normcase(abspath(U8(it->path())))) || name == ".bbmods") continue;
                    stack.push_back({U8(it->path()), r});
                } else {
                    File f;
                    f.rel = r;
                    f.lower = lower(r);
                    f.root = static_cast<int>(ri);
                    auto at = by_lower.find(f.lower);
                    if (at != by_lower.end()) {
                        files[at->second] = f;   // the update's file replaces the game's
                    } else {
                        by_lower[f.lower] = files.size();
                        files.push_back(f);
                    }
                    if (progress && ++n % 3000 == 0) progress("Reading game folder... " + std::to_string(n));
                }
            }
        }
    }
    for (const auto& [lw, idx] : by_lower) {
        const std::vector<std::string> lp = split(lw, '/');
        by_name[lp.back()].push_back(idx);
        const std::vector<std::string> rp = split(files[idx].rel, '/');
        for (std::size_t j = 1; j < lp.size(); ++j) dirs.emplace(join(lp, 0, j), join(rp, 0, j));
    }
    for (const auto& [ld, actual] : dirs) {
        (void)actual;
        const std::vector<std::string> lp = split(ld, '/');
        for (const std::string& c : lp) dirnames[c] = true;
        dirs_by_name[lp.back()].push_back(ld);
    }
    if (progress) progress("Game folder files: " + std::to_string(files.size()));
}

namespace {
bool ends_with_path(const std::string& full, const std::string& tail) {
    if (full == tail) return true;
    return full.size() > tail.size() && full.compare(full.size() - tail.size(), tail.size(), tail) == 0 && full[full.size() - tail.size() - 1] == '/';
}
}  // namespace

std::vector<std::size_t> GameIndex::find_files(const std::vector<std::string>& L, std::size_t from) const {
    std::vector<std::size_t> out;
    auto it = by_name.find(L.back());
    if (it == by_name.end()) return out;
    const std::string tail = join(L, from);
    for (std::size_t idx : it->second)
        if (ends_with_path(files[idx].lower, tail)) out.push_back(idx);
    return out;
}

std::vector<std::string> GameIndex::find_dirs(const std::vector<std::string>& L, std::size_t from, std::size_t to) const {
    std::vector<std::string> out;
    auto it = dirs_by_name.find(L[to - 1]);
    if (it == dirs_by_name.end()) return out;
    const std::string tail = join(L, from, to);
    for (const std::string& d : it->second)
        if (ends_with_path(d, tail)) out.push_back(d);
    return out;
}

// ------------------------------------------------------------------ matching
namespace {
struct Vote {
    std::vector<std::string> stripped, prefix_lower, prefix_actual;
    int count = 0;
};
}  // namespace

std::vector<Resolved> resolve_mod(const GameIndex& index, const std::string& mod_name, const std::vector<FileRef>& files,
                                  const std::map<std::string, std::string>& choices) {
    std::vector<Vote> votes;
    std::map<std::string, std::size_t> vote_at;
    auto vote_key = [](const std::vector<std::string>& stripped, const std::vector<std::string>& prefix) {
        return join(stripped) + "\x1f" + join(prefix);
    };
    auto stripped_ok = [&](const std::vector<std::string>& L, std::size_t k) {
        // The file name alone matches, but if the folder stripped from the front exists in the game
        // (chr/param.xml taken for system/param.xml) the file belongs elsewhere.
        if (k == 0 || L.size() - k >= 2) return true;
        for (std::size_t i = 0; i < k; ++i)
            if (index.dirnames.count(L[i])) return false;
        return true;
    };
    auto cast_vote = [&](const std::vector<std::string>& L, std::size_t n, std::size_t k, std::size_t file) {
        const std::vector<std::string> tl = split(index.files[file].lower, '/');
        const std::size_t plen = tl.size() - (n - k);
        const std::vector<std::string> stripped(L.begin(), L.begin() + static_cast<std::ptrdiff_t>(k));
        const std::vector<std::string> pre(tl.begin(), tl.begin() + static_cast<std::ptrdiff_t>(plen));
        const std::string key = vote_key(stripped, pre);
        auto it = vote_at.find(key);
        if (it == vote_at.end()) {
            Vote v;
            v.stripped = stripped;
            v.prefix_lower = pre;
            const std::vector<std::string> ra = split(index.files[file].rel, '/');
            v.prefix_actual.assign(ra.begin(), ra.begin() + static_cast<std::ptrdiff_t>(plen));
            vote_at[key] = votes.size();
            votes.push_back(std::move(v));
            it = vote_at.find(key);
        }
        ++votes[it->second].count;
    };

    struct Single { std::size_t ref, k, file; std::vector<std::string> L; };
    struct Multi { std::size_t ref; std::vector<std::string> L; std::vector<std::pair<std::size_t, std::size_t>> cands; };  // (k, file)
    struct None { std::size_t ref; std::vector<std::string> L; };
    std::vector<Single> singles;
    std::vector<Multi> multis;
    std::vector<None> nones;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::vector<std::string> L;
        for (const std::string& p : files[i].parts) L.push_back(lower(p));
        bool found = false;
        for (std::size_t k = 0; k < L.size() && !found; ++k) {
            const std::vector<std::size_t> lst = index.find_files(L, k);
            if (!lst.empty() && stripped_ok(L, k)) {
                found = true;
                if (lst.size() == 1) {
                    singles.push_back({i, k, lst[0], L});
                } else {
                    Multi m{i, L, {}};
                    for (std::size_t f : lst) m.cands.push_back({k, f});
                    multis.push_back(std::move(m));
                }
            }
        }
        if (!found) nones.push_back({i, L});
    }

    std::vector<Resolved> results(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) results[i].ref = files[i];

    for (const Single& s : singles) {
        cast_vote(s.L, s.L.size(), s.k, s.file);
        results[s.ref].status = Status::Exact;
        results[s.ref].rel = index.files[s.file].rel;
    }

    for (const Multi& m : multis) {
        Resolved& r = results[m.ref];
        const std::size_t n = m.L.size();
        const std::string ck = mod_name + "|" + join(m.L);
        r.key = ck;
        auto sv = choices.find(ck);
        const std::string saved = sv == choices.end() ? "" : sv->second;
        if (saved == kSkip) {
            r.status = Status::Skipped;
            continue;
        }
        std::ptrdiff_t pick = -1;
        if (!saved.empty()) {
            for (std::size_t c = 0; c < m.cands.size(); ++c)
                if (index.files[m.cands[c].second].lower == saved) {
                    pick = static_cast<std::ptrdiff_t>(c);
                    break;
                }
        }
        if (pick < 0) {
            struct Sc { int votes; std::size_t order; };
            std::vector<Sc> sc;
            for (std::size_t c = 0; c < m.cands.size(); ++c) {
                const auto [k, file] = m.cands[c];
                const std::vector<std::string> tl = split(index.files[file].lower, '/');
                const std::vector<std::string> stripped(m.L.begin(), m.L.begin() + static_cast<std::ptrdiff_t>(k));
                const std::vector<std::string> pre(tl.begin(), tl.begin() + static_cast<std::ptrdiff_t>(tl.size() - (n - k)));
                auto it = vote_at.find(vote_key(stripped, pre));
                sc.push_back({it == vote_at.end() ? 0 : votes[it->second].count, c});
            }
            std::stable_sort(sc.begin(), sc.end(), [](const Sc& a, const Sc& b) { return a.votes > b.votes; });
            if (sc[0].votes > 0 && (sc.size() == 1 || sc[0].votes > sc[1].votes)) pick = static_cast<std::ptrdiff_t>(sc[0].order);
        }
        if (pick >= 0) {
            r.status = Status::Picked;
            r.rel = index.files[m.cands[static_cast<std::size_t>(pick)].second].rel;
        } else {
            r.status = Status::Ambiguous;
            for (const auto& c : m.cands) r.cands.push_back(index.files[c.second].rel);
        }
    }

    for (const None& nn : nones) {
        Resolved& r = results[nn.ref];
        const std::vector<std::string>& P_ = files[nn.ref].parts;
        const std::vector<std::string>& L = nn.L;
        const std::size_t n = P_.size();
        const std::string ck = mod_name + "|" + join(L);
        r.key = ck;
        auto sv = choices.find(ck);
        const std::string saved = sv == choices.end() ? "" : sv->second;
        if (saved == kSkip) {
            r.status = Status::Skipped;
            continue;
        }
        if (doc_ext(P_.back())) {
            r.status = Status::Doc;
            continue;
        }
        // (a) A new file: if the tail of its folder path (chr, param/gameparam ...) exists in the game, it goes there;
        //     missing sub-folders are created. The longest tail first, then the one that strips fewer parent folders.
        bool found = false;
        std::vector<std::string> lst;
        std::size_t j = 0;
        for (std::size_t ln = n - 1; ln >= 1 && !found; --ln) {
            for (std::size_t k = 0; k + ln < n; ++k) {
                std::vector<std::string> d = index.find_dirs(L, k, k + ln);
                bool ok = k == 0 || ln >= 2;
                if (!ok) {
                    ok = true;
                    for (std::size_t i = 0; i < k; ++i)
                        if (index.dirnames.count(L[i])) ok = false;
                }
                if (!d.empty() && ok) {
                    lst = std::move(d);
                    j = k + ln;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            const std::string rest = join(P_, j);
            if (lst.size() == 1) {
                r.status = Status::Dir;
                r.rel = index.dirs.at(lst[0]) + "/" + rest;
            } else {
                std::vector<std::string> cands;
                for (const std::string& d : lst) cands.push_back(index.dirs.at(d) + "/" + rest);
                auto pk = std::find_if(cands.begin(), cands.end(), [&](const std::string& c) { return !saved.empty() && lower(c) == saved; });
                if (pk != cands.end()) {
                    r.status = Status::Dir;
                    r.rel = *pk;
                } else {
                    r.status = Status::Ambiguous;
                    r.cands = std::move(cands);
                }
            }
            continue;
        }
        // (b) No folder information at all: this mod's most used "stripped folder -> attached folder" rule.
        std::ptrdiff_t best = -1;
        for (std::size_t vi = 0; vi < votes.size(); ++vi) {
            const Vote& v = votes[vi];
            if (v.stripped.size() < n && std::equal(v.stripped.begin(), v.stripped.end(), L.begin())) {
                if (best < 0 || std::make_pair(v.count, v.stripped.size()) >
                                    std::make_pair(votes[static_cast<std::size_t>(best)].count, votes[static_cast<std::size_t>(best)].stripped.size()))
                    best = static_cast<std::ptrdiff_t>(vi);
            }
        }
        if (best >= 0) {
            const Vote& v = votes[static_cast<std::size_t>(best)];
            std::vector<std::string> rel = v.prefix_actual;
            rel.insert(rel.end(), P_.begin() + static_cast<std::ptrdiff_t>(v.stripped.size()), P_.end());
            r.status = Status::Mapped;
            r.rel = join(rel);
        } else {
            r.status = Status::Unmatched;
        }
    }
    return results;
}

// ------------------------------------------------------------------ plan
namespace {
std::string target_key(const std::string& root, const std::string& rel) { return normcase(root) + "|" + lower(rel); }
}  // namespace

const Target* Plan::find(const std::string& key) const {
    auto it = target_at.find(key);
    return it == target_at.end() ? nullptr : &targets[it->second].second;
}

Plan make_plan(const std::vector<ModSource>& mods, const GameIndex& index, const std::string& overlay_root,
               const std::map<std::string, std::string>& choices) {
    Plan plan;
    plan.overlay_root = abspath(overlay_root);
    for (const ModSource& mod : mods) {
        std::vector<Resolved> res = resolve_mod(index, mod.name, mod.files(), choices);
        plan.order.push_back(mod.name);
        for (Resolved& r : res) {
            if (r.status == Status::Ambiguous) plan.ambiguous.push_back({mod.name, r});
            if (r.rel.empty()) continue;
            if (!rel_safe(r.rel)) {  // safeguard: a path that leaves the overlay is not applied
                r.status = Status::Unmatched;
                r.rel.clear();
                continue;
            }
            const std::string key = target_key(plan.overlay_root, r.rel);
            auto at = plan.target_at.find(key);
            if (at == plan.target_at.end()) {
                Target t;
                t.root = plan.overlay_root;
                t.rel = r.rel;
                auto g = index.by_lower.find(lower(r.rel));
                if (g != index.by_lower.end()) t.orig = index.full_path(index.files[g->second]);
                plan.target_at[key] = plan.targets.size();
                plan.targets.push_back({key, std::move(t)});
                at = plan.target_at.find(key);
            }
            plan.targets[at->second].second.sources.push_back({mod.name, r.ref});
        }
        plan.per_mod.push_back({mod.name, std::move(res)});
    }
    return plan;
}

Bytes load_paramdef(const GameIndex& index) {
    Bytes out;
    const std::vector<std::string> L = {"paramdef.paramdefbnd.dcx"};
    const std::vector<std::size_t> f = index.find_files(L, 0);
    if (f.empty()) return out;
    std::string why;
    // The game's own folder (dvdroot_ps4/paramdef) when there are several.
    std::size_t pick = f[0];
    for (std::size_t i : f)
        if (index.files[i].lower == "dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx") pick = i;
    if (!read_file(index.full_path(index.files[pick]), &out, &why)) out.clear();
    return out;
}

const std::vector<std::string>& param_tables() {
    static const std::vector<std::string> t = {
        "actionbuttonparam", "aisoundparam", "atkparam_npc", "atkparam_pc", "behaviorparam", "behaviorparam_pc", "bullet",
        "calccorrectgraph", "charainitparam", "charmakemenulistitemparam", "charmakemenutopparam", "decalparam",
        "default_aistandardinfobank", "default_enemybehaviorbank", "dungeonfeatureparam", "dungeonsubfeatlotparam",
        "equipmtrlsetparam", "equipparamaccessory", "equipparamgoods", "equipparamprotector", "equipparamweapon",
        "facegenparam", "faceparam", "facerangeparam", "gameareaparam", "gamemapparam", "gemcategoryparam",
        "gemdropdopingparam", "gemdropmodifyparam", "gemeffectparam", "gemgenparam", "hitmtrlparam", "holygrailexparam",
        "itemlotlvdepparam", "itemlotparam", "knockbackparam", "lockcamparam", "magic", "menucolortableparam",
        "menupropertylayoutparam", "menupropertyspecparam", "menuvaluetableparam", "moveparam", "newmenucolortableparam",
        "npcparam", "npcthinkparam", "objactparam", "objectparam", "protectorgenparam", "qwcchange", "qwcjudge",
        "ragdollparam", "reinforceparamprotector", "reinforceparamweapon", "residentfxparam", "returnpointparam",
        "ritualrequiredmatparam", "shoplineupparam", "skeletonparam", "speffectparam", "speffectvfxparam", "talkparam",
        "throwparam", "weapongenparam", "wind"};
    return t;
}

namespace {
constexpr const char* kAnibndSuffix = ".anibnd.dcx";
constexpr const char* kParamSuffix = "gameparam.parambnd.dcx";
bool ends_with(const std::string& s, const char* suf) {
    const std::size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
}  // namespace

bool can_merge(const std::string& rel, bool param_ok) {
    const std::string r = lower(rel);
    if (ends_with(r, kAnibndSuffix)) return true;
    if (ends_with(r, kParamSuffix)) return param_ok;
    return false;
}

// ------------------------------------------------------------------ merging
namespace {

struct MergeResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> conflicts, warnings, diffs;
    Bytes out;
};

// The mods that touch a target, a mod appearing twice counting once (the later file).
std::vector<std::pair<std::string, const FileRef*>> tagged_sources(const Target& t) {
    std::vector<std::pair<std::string, const FileRef*>> in;
    for (const auto& [m, r] : t.sources) {
        in.erase(std::remove_if(in.begin(), in.end(), [&](const auto& x) { return x.first == m; }), in.end());
        in.push_back({m, &r});
    }
    return in;
}

std::vector<std::string> distinct_mods(const Target& t) {
    std::vector<std::string> names;
    for (const auto& s : t.sources)
        if (std::find(names.begin(), names.end(), s.first) == names.end()) names.push_back(s.first);
    return names;
}

std::string losers_text(const std::vector<std::string>& l) {
    std::string o;
    for (std::size_t i = 0; i < l.size(); ++i) o += (i ? ", " : "") + l[i];
    return o;
}

MergeResult do_merge(const std::string& rel, const std::string& orig_path, const Target& t, const MergeEnv& env, bool dry, const LogFn& log) {
    MergeResult res;
    Bytes original;
    if (!read_file(orig_path, &original, &res.error)) return res;
    const auto tagged = tagged_sources(t);
    const LogFn lg = log ? log : LogFn([](const std::string&) {});
    if (ends_with(lower(rel), kParamSuffix)) {
        std::vector<ParamMod> in;
        for (const auto& [m, ref] : tagged) {
            ParamMod pm;
            pm.label = m;
            if (!ref->read(&pm.file, &res.error)) return res;
            in.push_back(std::move(pm));
        }
        const std::set<std::string> allowed(param_tables().begin(), param_tables().end());
        ParamReport rep;
        if (!param_merge(original, env.paramdef, in, allowed, dry, rep, &res.out, &res.error, lg, env.compress_level)) return res;
        for (const ParamConflict& c : rep.conflicts) {
            std::string what = c.table;
            if (c.id >= 0) {
                what += " row " + std::to_string(c.id);
                if (!c.cells.empty()) what += " [" + join(c.cells, 0, std::string::npos, ',') + "]";
            } else {
                what += ": the whole table";
            }
            res.conflicts.push_back(what + ": " + c.winner + " wins (changes by " + losers_text(c.losers) + " are lost)");
        }
        res.warnings = rep.warnings;
        for (const auto& d : rep.diffs) res.diffs.push_back(d.first + " " + std::to_string(d.second));
    } else {
        std::vector<AnibndMod> in;
        for (const auto& [m, ref] : tagged) {
            AnibndMod am;
            am.label = m;
            if (!ref->read(&am.file, &res.error)) return res;
            in.push_back(std::move(am));
        }
        AnibndReport rep;
        if (!anibnd_merge(original, in, dry, rep, &res.out, &res.error, lg, env.compress_level)) return res;
        for (const AnibndConflict& c : rep.conflicts) {
            if (c.anim < 0) {
                res.conflicts.push_back(c.entry + ": cannot merge per animation -> " + c.winner + " wins the whole entry (changes by " +
                                        losers_text(c.losers) + " are lost)");
            } else {
                res.conflicts.push_back(c.entry + "  anim " + std::to_string(c.anim) + (c.index == 0 ? "" : "(#" + std::to_string(c.index) + ")") + ": " +
                                        c.winner + " wins (changes by " + losers_text(c.losers) + " are lost)");
            }
        }
        res.warnings = rep.warnings;
        for (const auto& d : rep.diffs) res.diffs.push_back(d.first + " " + std::to_string(d.second));
    }
    res.ok = true;
    return res;
}

}  // namespace

// ------------------------------------------------------------------ preview
bool Preview::has_conflicts() const {
    if (!overwritten.empty() || !errors.empty()) return true;
    for (const PreviewItem& i : items)
        if (!i.conflicts.empty() || !i.warnings.empty()) return true;
    return false;
}

std::vector<std::string> Preview::conflict_lines() const {
    std::vector<std::string> out;
    for (const PreviewItem& i : items)
        for (const std::string& c : i.conflicts) out.push_back(c);
    for (const PreviewItem& i : items)
        for (const std::string& w : i.warnings) out.push_back("Note: " + w);
    for (const auto& [rel, names] : overwritten) {
        std::vector<std::string> rest(names.begin(), names.end() - 1);
        out.push_back(rel + ": overwritten wholesale -> only " + names.back() + " is applied (" + losers_text(rest) + " is ignored)");
    }
    for (const std::string& e : errors) out.push_back("Error: " + e);
    return out;
}

std::vector<std::string> Preview::diagnostics() const {
    std::vector<std::string> out;
    for (const auto& [rel, names] : mergeable) out.push_back("Merge target: " + rel + "  <-  " + join(names, 0, std::string::npos, '+'));
    for (const PreviewItem& i : items) {
        out.push_back("Merge base file: " + i.base);
        out.push_back("  Entries differing from the base, per mod: " + losers_text(i.diffs));
    }
    if (items.empty()) out.push_back("No anibnd / gameparam file is changed by two or more mods");
    return out;
}

Preview preview_merges(const Plan& plan, const MergeEnv& env, const LogFn& log) {
    Preview pv;
    const bool param_ok = !env.paramdef.empty();
    for (const auto& [key, t] : plan.targets) {
        (void)key;
        const std::vector<std::string> names = distinct_mods(t);
        if (can_merge(t.rel, param_ok) || ends_with(lower(t.rel), kParamSuffix) || ends_with(lower(t.rel), kAnibndSuffix)) pv.mergeable.push_back({t.rel, names});
        if (names.size() < 2) continue;
        if (!can_merge(t.rel, param_ok) || t.orig.empty()) {
            pv.overwritten.push_back({t.rel, names});  // no merge, or no original to merge against: the last wins whole
            continue;
        }
        PreviewItem item;
        item.rel = t.rel;
        item.mods = names;
        item.base = t.orig;
        MergeResult r = do_merge(t.rel, t.orig, t, env, true, log);
        if (!r.ok) {
            item.error = r.error;
            pv.errors.push_back(t.rel + ": " + r.error);
        }
        item.conflicts = r.conflicts;
        item.warnings = r.warnings;
        item.diffs = r.diffs;
        pv.items.push_back(std::move(item));
    }
    return pv;
}

// ------------------------------------------------------------------ state (the Python loader's state.json)
namespace {

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::string> okeys;   // an object: its keys and, in step, its values
    std::vector<Json> ovals;
    const Json* get(const std::string& k) const {
        for (std::size_t i = 0; i < okeys.size(); ++i)
            if (okeys[i] == k) return &ovals[i];
        return nullptr;
    }
};

struct JsonParser {
    const std::string& t;
    std::size_t i = 0;
    bool ok = true;
    explicit JsonParser(const std::string& text) : t(text) {}
    void ws() {
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    }
    static void put_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i >= t.size()) { ok = false; return 0; }
            const char c = t[i++];
            v = v * 16 + static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = false, 0));
        }
        return v;
    }
    std::string str() {
        std::string o;
        ++i;  // the opening quote
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) { ok = false; break; }
            c = t[i++];
            switch (c) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        const unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(o, cp);
                    break;
                }
                default: o += c;
            }
        }
        if (i >= t.size()) ok = false;
        ++i;
        return o;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (i >= t.size() || depth > 32) { ok = false; return j; }
        const char c = t[i];
        if (c == '{') {
            j.kind = Json::Obj;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') { ++i; return j; }
            while (ok) {
                ws();
                if (i >= t.size() || t[i] != '"') { ok = false; break; }
                std::string k = str();
                ws();
                if (i >= t.size() || t[i] != ':') { ok = false; break; }
                ++i;
                Json v = value(depth + 1);
                j.okeys.push_back(std::move(k));
                j.ovals.push_back(std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; break; }
                ok = false;
            }
        } else if (c == '[') {
            j.kind = Json::Arr;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') { ++i; return j; }
            while (ok) {
                j.a.push_back(value(depth + 1));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; break; }
                ok = false;
            }
        } else if (c == '"') {
            j.kind = Json::Str;
            j.s = str();
        } else if (t.compare(i, 4, "true") == 0) {
            j.kind = Json::Bool; j.b = true; i += 4;
        } else if (t.compare(i, 5, "false") == 0) {
            j.kind = Json::Bool; j.b = false; i += 5;
        } else if (t.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            const std::size_t st = i;
            while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '-' || t[i] == '+' || t[i] == '.' || t[i] == 'e' || t[i] == 'E')) ++i;
            if (i == st) { ok = false; return j; }
            j.kind = Json::Num;
            j.n = std::atof(t.substr(st, i - st).c_str());
        }
        return j;
    }
};

std::string json_quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

struct StateEntry {
    std::string root, rel, sig;
    bool backup = false;
    std::vector<std::string> mods;
};
struct State {
    std::vector<std::pair<std::string, StateEntry>> entries;
    std::vector<std::string> order;
    StateEntry* find(const std::string& k) {
        for (auto& e : entries)
            if (e.first == k) return &e.second;
        return nullptr;
    }
    void erase(const std::string& k) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& e) { return e.first == k; }), entries.end());
    }
};

std::string state_file(const std::string& state_dir) { return U8(P(state_dir) / "state.json"); }

State load_state(const std::string& state_dir) {
    State st;
    Bytes raw;
    std::string why;
    if (!read_file(state_file(state_dir), &raw, &why)) return st;
    std::string text(raw.begin(), raw.end());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    JsonParser p(text);
    const Json root = p.value();
    if (!p.ok || root.kind != Json::Obj) return st;
    if (const Json* e = root.get("entries"); e && e->kind == Json::Obj) {
        for (std::size_t ei = 0; ei < e->okeys.size(); ++ei) {
            const std::string& k = e->okeys[ei];
            const Json& v = e->ovals[ei];
            if (v.kind != Json::Obj) continue;
            StateEntry se;
            if (auto* x = v.get("root"); x && x->kind == Json::Str) se.root = x->s;
            if (auto* x = v.get("rel"); x && x->kind == Json::Str) se.rel = x->s;
            if (auto* x = v.get("sig"); x && x->kind == Json::Str) se.sig = x->s;
            if (auto* x = v.get("backup"); x && x->kind == Json::Bool) se.backup = x->b;
            if (auto* x = v.get("mods"); x && x->kind == Json::Arr)
                for (const Json& m : x->a)
                    if (m.kind == Json::Str) se.mods.push_back(m.s);
            if (!se.root.empty() && !se.rel.empty()) st.entries.push_back({k, std::move(se)});
        }
    }
    if (const Json* o = root.get("order"); o && o->kind == Json::Arr)
        for (const Json& m : o->a)
            if (m.kind == Json::Str) st.order.push_back(m.s);
    return st;
}

bool save_state(const std::string& state_dir, const State& st, std::string* error) {
    std::string o = "{\n \"version\": 1,\n \"entries\": {";
    bool first = true;
    for (const auto& [k, e] : st.entries) {
        o += std::string(first ? "\n  " : ",\n  ") + json_quote(k) + ": {\n   \"root\": " + json_quote(e.root) + ",\n   \"rel\": " + json_quote(e.rel) +
             ",\n   \"sig\": " + json_quote(e.sig) + ",\n   \"backup\": " + (e.backup ? "true" : "false") + ",\n   \"mods\": [";
        for (std::size_t i = 0; i < e.mods.size(); ++i) o += (i ? ", " : "") + json_quote(e.mods[i]);
        o += "]\n  }";
        first = false;
    }
    o += first ? "}" : "\n }";
    o += ",\n \"order\": [";
    for (std::size_t i = 0; i < st.order.size(); ++i) o += (i ? ", " : "") + json_quote(st.order[i]);
    o += "],\n \"mode\": \"overlay\"\n}\n";
    return write_atomic(state_file(state_dir), Bytes(o.begin(), o.end()), error);
}

std::string backup_path(const std::string& state_dir, const std::string& root, const std::string& rel) {
    fs::path p = P(state_dir) / "backup" / P(sha1_hex8(normcase(root)));
    for (const std::string& c : split(rel, '/')) p /= P(c);
    return U8(p);
}

// Removes empty folders from `d` up to (not including) `stop`.
void prune_empty(const std::string& d0, const std::string& stop0) {
    const std::string stop = normcase(abspath(stop0));
    std::string d = abspath(d0);
    while (normcase(d) != stop && d.size() > stop.size()) {
        std::error_code ec;
        if (!fs::remove(P(d), ec) || ec) break;   // only an empty folder goes
        d = abspath(U8(P(d).parent_path()));
    }
}

// Puts the entry's file back as it was: the saved original, or nothing.
void revert_entry(const StateEntry& ent, const std::string& state_dir) {
    if (!rel_safe(ent.rel)) throw std::runtime_error("unsafe path in the record: " + ent.rel);
    const std::string dst = U8(join_rel(ent.root, ent.rel));
    const std::string bk = backup_path(state_dir, ent.root, ent.rel);
    if (ent.backup) {
        if (!exists(bk)) throw std::runtime_error("cannot restore the original because there is no backup (the record is kept): " + ent.rel);
        std::string why;
        if (!copy_replace(bk, dst, &why)) throw std::runtime_error(why);
        std::error_code ec;
        fs::remove(P(bk), ec);
        prune_empty(U8(P(bk).parent_path()), U8(P(state_dir) / "backup"));
    } else if (exists(dst)) {
        std::error_code ec;
        fs::remove(P(dst), ec);
        if (ec) throw std::runtime_error("cannot remove " + dst + ": " + ec.message());
        prune_empty(U8(P(dst).parent_path()), ent.root);
    }
}

std::string file_sig(const std::string& path) {
    std::error_code ec;
    const auto sz = fs::file_size(P(path), ec);
    if (ec) return "none";
    const auto mt = fs::last_write_time(P(path), ec);
    return std::to_string(static_cast<unsigned long long>(sz)) + ":" + std::to_string(static_cast<long long>(mt.time_since_epoch().count()));
}

// Part of every merged file's signature: when the merge's inputs besides the mods change, it is redone.
constexpr const char* kMergeVersion = "cpp1";

std::string now_text() {
    char b[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return b;
}

// Merge-only log: <state>/merge.log, started only by an apply in which a merge or a conflict happens.
struct MergeLog {
    std::string path, dir;
    std::ofstream f;
    std::vector<std::string> pending;
    MergeLog(const std::string& state_dir) : path(U8(P(state_dir) / "merge.log")), dir(state_dir) {}
    void head(const std::string& text, const std::string& more) { pending = {"", "=== " + now_text() + "  " + text + " ===", more}; }
    void write(const std::string& line) {
        if (!f.is_open()) {
            std::error_code ec;
            fs::create_directories(P(dir), ec);
            f.open(P(path), std::ios::app | std::ios::binary);
            for (const std::string& l : pending) f << l << "\n";
            pending.clear();
        }
        f << line << "\n";
        f.flush();
    }
    void write_if_started(const std::string& line) {
        if (f.is_open()) write(line);
    }
};

}  // namespace

// ------------------------------------------------------------------ apply / revert
ApplyStats apply_plan(const Plan& plan, const std::string& state_dir, const MergeEnv& env, const LogFn& log_in,
                      const std::function<void(int, int)>& progress) {
    ApplyStats stats;
    const LogFn log = log_in ? log_in : LogFn([](const std::string&) {});
    std::error_code ec;
    fs::create_directories(P(state_dir), ec);
    State st = load_state(state_dir);
    MergeLog mlog(state_dir);
    const bool param_ok = !env.paramdef.empty();
    const std::string env_sig = std::string(kMergeVersion) + (param_ok ? "p" : "-") + std::to_string(env.compress_level);
    auto last_save = std::chrono::steady_clock::now();
    mlog.head("Apply started (mods: " + (plan.order.empty() ? std::string("none") : join(plan.order, 0, std::string::npos, '>')) + ")",
              "Priority: the mod later in the list wins");
    auto say_error = [&](const std::string& msg) {
        ++stats.error;
        log("  Error " + msg);
    };
    // 1) What is recorded but no longer wanted goes back to the original.
    {
        std::vector<std::string> gone;
        for (const auto& [k, e] : st.entries)
            if (!plan.find(k)) gone.push_back(k);
        for (const std::string& k : gone) {
            StateEntry* e = st.find(k);
            try {
                revert_entry(*e, state_dir);
                ++stats.reverted;
                st.erase(k);
            } catch (const std::exception& ex) {
                say_error("(restore failed) " + e->rel + ": " + ex.what());
            }
        }
    }
    // 2) The wanted state.
    const int total = static_cast<int>(plan.targets.size());
    int done = 0;
    for (const auto& [k, t] : plan.targets) {
        if (progress) progress(++done, total);
        if (!rel_safe(t.rel)) {
            say_error(t.rel + ": unsafe path, skipped");
            continue;
        }
        const std::string dst = U8(join_rel(t.root, t.rel));
        StateEntry* ent = st.find(k);
        const std::vector<std::string> names = distinct_mods(t);
        std::vector<std::string> all_names;
        for (const auto& s : t.sources) all_names.push_back(s.first);
        const bool merge_it = names.size() >= 2 && can_merge(t.rel, param_ok) && !t.orig.empty();
        std::string sigs;
        for (const auto& [m, r] : t.sources) sigs += m + "=" + r.sig + "|";
        const std::string last_sig = t.sources.back().first + "=" + t.sources.back().second.sig;
        const std::string sig = merge_it ? "M|" + sigs + file_sig(t.orig) + "|" + env_sig : "F|" + last_sig;
        std::vector<std::string> sorted_names = names;
        std::sort(sorted_names.begin(), sorted_names.end());
        if (ent && ent->sig == sig && exists(dst)) {
            ent->mods = sorted_names;
            ++stats.same;
            continue;
        }
        try {
            const bool existed = ent ? ent->backup : exists(dst);
            if (!ent && existed) {  // the first time a file that was already there is overwritten, keep it
                const std::string bk = backup_path(state_dir, t.root, t.rel);
                if (!exists(bk)) {   // a backup left by an interrupted apply is the real original: not overwritten
                    std::string why;
                    if (!copy_replace(dst, bk, &why)) throw std::runtime_error("cannot save the original: " + why);
                }
            }
            bool merged = false;
            if (merge_it) {
                const auto tagged = tagged_sources(t);
                std::string label;
                for (std::size_t i = 0; i < tagged.size(); ++i) label += (i ? " + " : "") + tagged[i].first;
                log("Merge: " + t.rel + "  (" + label + ")");
                mlog.write("Merge: " + t.rel + "  (" + label + ")");
                mlog.write("  Base original: " + t.orig);
                MergeResult r = do_merge(t.rel, t.orig, t, env, false, [&](const std::string& line) {
                    log(line);
                    mlog.write(line);
                });
                std::string why = r.error;
                if (r.ok && !r.out.empty() && write_atomic(dst, r.out, &why)) {
                    merged = true;
                    ++stats.merged;
                } else {
                    const std::string m = "  Merge failed (" + why + ") -> applying the last mod file as is";
                    log(m);
                    mlog.write(m);
                }
            }
            if (!merged) {
                if (names.size() > 1) {
                    std::vector<std::string> rest(names.begin(), names.end() - 1);
                    const std::string m = "Conflict: " + t.rel + "  (" + losers_text(rest) + " -> " + names.back() + " applied)";
                    log(m);
                    mlog.write(m);
                    ++stats.conflict;
                }
                std::string why;
                if (!t.sources.back().second.write_to(dst, &why)) throw std::runtime_error(why);
                ++stats.copied;
            }
            StateEntry ne;
            ne.root = t.root;
            ne.rel = t.rel;
            ne.sig = sig;
            ne.backup = existed;
            ne.mods = sorted_names;
            if (ent) *ent = ne;
            else st.entries.push_back({k, ne});
            // Saved from time to time, so what was written is known even if this is cut off midway.
            if (std::chrono::steady_clock::now() - last_save > std::chrono::milliseconds(500)) {
                st.order = plan.order;
                std::string why;
                save_state(state_dir, st, &why);
                last_save = std::chrono::steady_clock::now();
            }
        } catch (const std::exception& ex) {
            say_error(t.rel + ": " + ex.what());
        }
    }
    st.order = plan.order;
    std::string why;
    if (!save_state(state_dir, st, &why)) say_error("the record could not be saved: " + why);
    mlog.write_if_started("Result: same " + std::to_string(stats.same) + ", copied " + std::to_string(stats.copied) + ", merged " +
                          std::to_string(stats.merged) + ", conflict " + std::to_string(stats.conflict) + ", reverted " +
                          std::to_string(stats.reverted) + ", error " + std::to_string(stats.error));
    return stats;
}

int revert_all(const std::string& state_dir, const LogFn& log_in) {
    const LogFn log = log_in ? log_in : LogFn([](const std::string&) {});
    State st = load_state(state_dir);
    int n = 0;
    std::vector<std::string> keys;
    for (const auto& e : st.entries) keys.push_back(e.first);
    for (const std::string& k : keys) {
        StateEntry* e = st.find(k);
        try {
            revert_entry(*e, state_dir);
            st.erase(k);
            ++n;
        } catch (const std::exception& ex) {
            log("  Error (restore failed) " + e->rel + ": " + ex.what());
        }
    }
    st.order.clear();
    std::string why;
    if (!save_state(state_dir, st, &why)) log("  Error: the record could not be saved: " + why);
    log("Everything reverted: " + std::to_string(n) + " files");
    return n;
}

std::pair<std::vector<std::string>, std::vector<std::string>> applied_info(const std::string& state_dir) {
    State st = load_state(state_dir);
    std::vector<std::string> names;
    for (const auto& e : st.entries)
        for (const std::string& m : e.second.mods)
            if (std::find(names.begin(), names.end(), m) == names.end()) names.push_back(m);
    return {names, st.order};
}

std::string read_merge_log(const std::string& state_dir, std::size_t tail_bytes) {
    std::ifstream f(P(U8(P(state_dir) / "merge.log")), std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const std::size_t n = static_cast<std::size_t>(f.tellg());
    const std::size_t from = n > tail_bytes ? n - tail_bytes : 0;
    f.seekg(static_cast<std::streamoff>(from));
    std::string s(n - from, '\0');
    f.read(&s[0], static_cast<std::streamsize>(s.size()));
    return s;
}

// ------------------------------------------------------------------ paths
bool paths_overlap(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    const std::string x = normcase(abspath(a)), y = normcase(abspath(b));
    const char sep = static_cast<char>(fs::path::preferred_separator);
    return x == y || x.rfind(y + sep, 0) == 0 || y.rfind(x + sep, 0) == 0;
}

FolderProblem check_folders(const std::string& app0, const std::string& overlay, const std::string& mods_dir) {
    if (overlay.empty()) return FolderProblem::OverlayEmpty;
    if (mods_dir.empty()) return FolderProblem::ModsEmpty;
    if (paths_overlap(overlay, mods_dir)) return FolderProblem::OverlayInsideOrContainsMods;
    if (!app0.empty()) {
        if (paths_overlap(overlay, app0)) return FolderProblem::OverlayInsideOrContainsGame;
        const std::string up = find_update_folder(app0);
        if (!up.empty() && paths_overlap(overlay, up)) return FolderProblem::OverlayInsideOrContainsGame;
    }
    return FolderProblem::None;
}

bool check_bloodborne(const std::string& app0, std::string* message) {
    auto say = [&](const std::string& m) {
        if (message) *message = m;
    };
    if (app0.empty() || !is_dir(app0)) {
        say("Game folder does not exist");
        return false;
    }
    std::error_code ec;
    for (fs::directory_iterator it(P(app0), ec), end; !ec && it != end; it.increment(ec))
        if (lower(U8(it->path().filename())) == "dvdroot_ps4") {
            say("Bloodborne folder found");
            return true;
        }
    say("Not a Bloodborne game folder (no dvdroot_ps4 folder)");
    return false;
}

}  // namespace mods
