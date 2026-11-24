#include "mods/param_merge.h"

#include "gcn/container.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace mods {
namespace {

using Bytes = std::vector<std::uint8_t>;

struct ParamError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <typename T>
T rd(const Bytes& b, std::size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

template <typename T>
void wr(Bytes& b, std::size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(T));
}

// ------------------------------------------------------------- cell layout

// One cell of a row: a run of bytes (nbits == 0) or a bit field of a 1/2/4/8
// byte unit, from `shift` upward.
struct Cell {
    std::int64_t off = 0, size = 0;
    int shift = 0, nbits = 0;
    std::string name;
};

struct Layout {
    std::string ptype;
    std::vector<Cell> cells;
    std::int64_t row_size = 0;
};

struct FieldDef {
    std::string type, name;
    int arr = 0, bits = 0;
};

int type_size(const std::string& t) {
    static const std::map<std::string, int> sizes = {
        {"s8", 1},  {"u8", 1},  {"s16", 2},     {"u16", 2},      {"s32", 4},     {"u32", 4},   {"f32", 4},
        {"f64", 8}, {"dummy8", 1}, {"fixstr", 1}, {"fixstrW", 2}, {"b32", 4},     {"angle32", 4}, {"s64", 8},
        {"u64", 8}};
    const auto it = sizes.find(t);
    return it == sizes.end() ? -1 : it->second;
}

// Bit fields of the same size pack from the low bits up; anything else ends the unit.
Layout build_layout(const std::string& ptype, const std::vector<FieldDef>& fields) {
    Layout lay;
    lay.ptype = ptype;
    std::int64_t off = 0;
    bool have_unit = false;
    std::int64_t unit_off = 0, unit_size = 0;
    int unit_bits = 0;
    for (const FieldDef& f : fields) {
        const int size = type_size(f.type);
        if (size < 0) throw ParamError("unknown field type: " + f.type);
        if (f.bits) {
            if (have_unit && unit_size == size && unit_bits + f.bits <= size * 8) {
                lay.cells.push_back({unit_off, size, unit_bits, f.bits, f.name});
                unit_bits += f.bits;
            } else {
                if (have_unit) off = unit_off + unit_size;
                have_unit = true;
                unit_off = off;
                unit_size = size;
                unit_bits = f.bits;
                lay.cells.push_back({off, size, 0, f.bits, f.name});
            }
        } else {
            if (have_unit) {
                off = unit_off + unit_size;
                have_unit = false;
            }
            const std::int64_t n = static_cast<std::int64_t>(size) * (f.arr ? f.arr : 1);
            lay.cells.push_back({off, n, 0, 0, f.name});
            off += n;
        }
    }
    if (have_unit) off = unit_off + unit_size;
    lay.row_size = off;
    return lay;
}

Layout blob_layout(std::int64_t row_size) {
    Layout lay;
    lay.row_size = row_size;
    lay.cells.push_back({0, row_size, 0, 0, "(whole row)"});
    return lay;
}

std::uint64_t cell_value(const Bytes& row, const Cell& c) {
    std::uint64_t v = 0;
    std::memcpy(&v, row.data() + c.off, static_cast<std::size_t>(c.size));  // little endian host
    return v;
}

std::uint64_t cell_mask(const Cell& c) { return c.nbits >= 64 ? ~0ull : ((1ull << c.nbits) - 1); }

bool cell_differs(const Bytes& a, const Bytes& b, const Cell& c) {
    if (c.nbits) return ((cell_value(a, c) >> c.shift) & cell_mask(c)) != ((cell_value(b, c) >> c.shift) & cell_mask(c));
    return std::memcmp(a.data() + c.off, b.data() + c.off, static_cast<std::size_t>(c.size)) != 0;
}

// row[c] = src[c]
void cell_copy(Bytes& row, const Bytes& src, const Cell& c) {
    if (c.nbits) {
        const std::uint64_t mask = cell_mask(c) << c.shift;
        const std::uint64_t v = (cell_value(row, c) & ~mask) | ((((cell_value(src, c) >> c.shift) & cell_mask(c)) << c.shift) & mask);
        std::memcpy(row.data() + c.off, &v, static_cast<std::size_t>(c.size));
    } else {
        std::memcpy(row.data() + c.off, src.data() + c.off, static_cast<std::size_t>(c.size));
    }
}

// ------------------------------------------------------------- paramdef

// "name", "name[3]", "name:1", "name = 5": what the definition string of a field can hold.
bool parse_field_name(const std::string& s, FieldDef& out) {
    std::size_t i = 0;
    auto ws = [&] {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    };
    auto number = [&](int& v) {
        const std::size_t st = i;
        long n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && n < 100000000) n = n * 10 + (s[i++] - '0');
        v = static_cast<int>(n);
        return i > st;
    };
    ws();
    const std::size_t st = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '[' && s[i] != ':' && s[i] != '=') ++i;
    if (i == st) return false;
    out.name = s.substr(st, i - st);
    ws();
    if (i < s.size() && s[i] == '[') {
        ++i;
        if (!number(out.arr) || i >= s.size() || s[i] != ']') return false;
        ++i;
        ws();
    }
    if (i < s.size() && s[i] == ':') {
        ++i;
        ws();
        if (!number(out.bits)) return false;
        ws();
    }
    if (i < s.size() && s[i] == '=') {
        ++i;
        return i < s.size();
    }
    return i == s.size();
}

std::string cstr(const Bytes& d, std::size_t at, std::size_t max) {
    std::size_t n = 0;
    while (n < max && at + n < d.size() && d[at + n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(d.data() + at), n);
}

std::string utf8_from_utf16le(const Bytes& n) {
    std::string s;
    for (std::size_t i = 0; i + 1 < n.size(); i += 2) {
        std::uint32_t c = n[i] | (n[i + 1] << 8);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n.size()) {
            const std::uint32_t d = n[i + 2] | (n[i + 3] << 8);
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) {
            s.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (c >> 6)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (c >> 12)));
            s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (c >> 18)));
            s.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return s;
}

// "Folder\\EquipParamWeapon.param" -> "EquipParamWeapon.param"
std::string base_name(const gcn::Bnd4File& f) {
    std::string n = utf8_from_utf16le(f.name);
    std::replace(n.begin(), n.end(), '\\', '/');
    const std::size_t at = n.rfind('/');
    return at == std::string::npos ? n : n.substr(at + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads a DCX (or a bare BND4) into an archive the merge can use.
bool read_archive(const Bytes& file, bool must_be_dcx, gcn::Bnd4Archive& out, std::string* error) {
    if (must_be_dcx && (file.size() < 4 || std::memcmp(file.data(), "DCX\0", 4) != 0)) {
        if (error) *error = "this must be a DCX file";
        return false;
    }
    std::string why;
    const Bytes raw = gcn::dcx_decompress(file, &why);
    if (raw.empty()) {
        if (error) *error = why.empty() ? "empty file" : why;
        return false;
    }
    if (!gcn::bnd4_read(raw, out, &why)) {
        if (error) *error = why;
        return false;
    }
    if (out.head.size() < 0x32 || out.head[0x31] != 0x74 || out.entry_size != 0x24 || !out.unicode) {
        if (error) *error = "a BND4 layout this does not read";
        return false;
    }
    return true;
}

// One PARAMDEF (64-bit, version 201 as the game ships it) -> its param type and layout.
// A field is 0xD0 bytes: the type at +0x40 ("u8"), the name at +0x90 ("flag:1", "pad[3]").
Layout read_paramdef(const Bytes& d) {
    if (d.size() < 0x38) throw ParamError("PARAMDEF too short");
    const std::uint16_t count = rd<std::uint16_t>(d, 8), esz = rd<std::uint16_t>(d, 0xA);
    const std::uint16_t version = rd<std::uint16_t>(d, 0x2E);
    const std::int64_t foff = rd<std::int64_t>(d, 0x30);
    if (version != 201 || esz < 0xD0) throw ParamError("a PARAMDEF layout this does not read");
    if (foff < 0 || static_cast<std::uint64_t>(foff) + static_cast<std::uint64_t>(count) * esz > d.size())
        throw ParamError("PARAMDEF fields out of range");
    std::vector<FieldDef> fields;
    for (int k = 0; k < count; ++k) {
        const std::size_t o = static_cast<std::size_t>(foff) + static_cast<std::size_t>(k) * esz;
        FieldDef f;
        f.type = cstr(d, o + 0x40, 8);
        if (!parse_field_name(cstr(d, o + 0x90, 0x20), f)) throw ParamError("cannot read field definition: " + f.type + " " + cstr(d, o + 0x90, 0x20));
        fields.push_back(std::move(f));
    }
    return build_layout(cstr(d, 0x0C, 0x20), fields);
}

// ------------------------------------------------------------- PARAM

struct Row {
    std::int32_t id = 0;
    Bytes data;
    bool has_name = false;
    Bytes name;
};
using RowP = std::shared_ptr<Row>;

// A PARAM file (64-bit offsets): header 0x40, row table (24 bytes each), row data, row names, tail.
struct Param {
    Bytes hdr;
    std::string ptype;
    bool unicode_names = false;
    std::int64_t row_size = 0;
    std::vector<RowP> rows;
    Bytes tail;

    explicit Param(const Bytes& data) {
        if (data.size() < 0x40) throw ParamError("PARAM too short");
        hdr.assign(data.begin(), data.begin() + 0x40);
        const std::int32_t strings_off = rd<std::int32_t>(data, 0);
        const std::uint16_t count = rd<std::uint16_t>(data, 0x0A);
        const std::int64_t data_start = rd<std::int64_t>(data, 0x30);
        ptype = cstr(data, 0x0C, 0x20);
        unicode_names = (data[0x2E] & 1) != 0;
        if (data_start != 0x40 + 24 * static_cast<std::int64_t>(count)) throw ParamError("row table / data start is not where expected");
        if (strings_off < data_start || static_cast<std::size_t>(strings_off) > data.size())
            throw ParamError("PARAM is cut off or damaged (string offset outside the file)");
        if (count == 0) {
            tail.assign(data.begin() + strings_off, data.end());
            row_size = 0;
            return;
        }
        const std::int64_t total = strings_off - data_start;
        if (total % count) throw ParamError("row data size does not divide by the row count");
        row_size = total / count;
        std::int64_t end_names = strings_off;
        for (int i = 0; i < count; ++i) {
            const std::size_t at = 0x40 + 24 * static_cast<std::size_t>(i);
            auto r = std::make_shared<Row>();
            r->id = rd<std::int32_t>(data, at);
            const std::int64_t doff = rd<std::int64_t>(data, at + 8), noff = rd<std::int64_t>(data, at + 16);
            if (doff != data_start + i * row_size) throw ParamError("row data is not laid out one after another");
            if (noff) {
                if (noff < strings_off || static_cast<std::size_t>(noff) >= data.size()) throw ParamError("row name outside the file (PARAM is damaged)");
                std::size_t j = static_cast<std::size_t>(noff);
                if (unicode_names) {
                    while (!(j + 2 <= data.size() && data[j] == 0 && data[j + 1] == 0)) {
                        j += 2;
                        if (j + 2 > data.size()) throw ParamError("row name does not end (PARAM is damaged)");
                    }
                    end_names = std::max<std::int64_t>(end_names, static_cast<std::int64_t>(j) + 2);
                } else {
                    while (data[j] != 0) {
                        ++j;
                        if (j >= data.size()) throw ParamError("row name does not end (PARAM is damaged)");
                    }
                    end_names = std::max<std::int64_t>(end_names, static_cast<std::int64_t>(j) + 1);
                }
                r->has_name = true;
                r->name.assign(data.begin() + noff, data.begin() + static_cast<std::int64_t>(j));
            }
            r->data.assign(data.begin() + doff, data.begin() + doff + row_size);
            rows.push_back(std::move(r));
        }
        tail.assign(data.begin() + end_names, data.end());
    }

    Bytes write() const {
        const std::size_t n = rows.size();
        if (n > 0xFFFF) throw ParamError("too many rows for a PARAM");
        const std::size_t data_start = 0x40 + 24 * n;
        const std::size_t rs = n ? rows[0]->data.size() : 0;
        const std::size_t strings_off = data_start + rs * n;
        const std::size_t term = unicode_names ? 2 : 1;
        Bytes tbl(24 * n), names, body;
        body.reserve(rs * n);
        for (std::size_t i = 0; i < n; ++i) {
            const Row& r = *rows[i];
            if (r.data.size() != rs) throw ParamError("rows differ in size");
            std::int64_t noff = 0;
            if (r.has_name) {
                noff = static_cast<std::int64_t>(strings_off + names.size());
                names.insert(names.end(), r.name.begin(), r.name.end());
                names.insert(names.end(), term, 0);
            }
            wr<std::int32_t>(tbl, 24 * i, r.id);
            wr<std::int32_t>(tbl, 24 * i + 4, 0);
            wr<std::int64_t>(tbl, 24 * i + 8, static_cast<std::int64_t>(data_start + i * rs));
            wr<std::int64_t>(tbl, 24 * i + 16, noff);
            body.insert(body.end(), r.data.begin(), r.data.end());
        }
        Bytes h = hdr;
        wr<std::int32_t>(h, 0, static_cast<std::int32_t>(strings_off));
        wr<std::uint16_t>(h, 0x0A, static_cast<std::uint16_t>(n));
        wr<std::int64_t>(h, 0x30, static_cast<std::int64_t>(data_start));
        Bytes out = std::move(h);
        out.insert(out.end(), tbl.begin(), tbl.end());
        out.insert(out.end(), body.begin(), body.end());
        out.insert(out.end(), names.begin(), names.end());
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    }
};

// A name written for one encoding (UTF-16 or Shift-JIS) for a table of the other.
// Only plain ASCII converts here; anything else leaves the name empty (and counts).
Bytes convert_name(const Bytes& name, bool from_unicode, bool to_unicode, bool& lost) {
    if (from_unicode == to_unicode) return name;
    Bytes out;
    if (from_unicode) {
        if (name.size() % 2) {
            lost = true;
            return {};
        }
        for (std::size_t i = 0; i < name.size(); i += 2) {
            if (name[i + 1] != 0 || name[i] >= 0x80) {
                lost = true;
                return {};
            }
            out.push_back(name[i]);
        }
    } else {
        for (std::uint8_t c : name) {
            if (c >= 0x80) {
                lost = true;
                return {};
            }
            out.push_back(c);
            out.push_back(0);
        }
    }
    return out;
}

// ------------------------------------------------------------- merging one table

using Key = std::pair<std::int32_t, int>;  // (row ID, which of several rows with that ID)

// Rows by (ID, nth with that ID); the original has tables with repeated IDs.
struct Keyed {
    std::vector<std::pair<Key, RowP>> order;
    std::map<Key, RowP> index;
    explicit Keyed(const std::vector<RowP>& rows) {
        std::map<std::int32_t, int> seen;
        for (const RowP& r : rows) {
            const Key k{r->id, seen[r->id]++};
            order.emplace_back(k, r);
            index[k] = r;
        }
    }
    const RowP* find(const Key& k) const {
        const auto it = index.find(k);
        return it == index.end() ? nullptr : &it->second;
    }
};

// Do the mod's rows differ from the original's in their data (names and file layout aside)?
bool semantic_diff(const Keyed& vk, const Keyed& pk) {
    if (vk.index.size() != pk.index.size()) return true;
    for (const auto& kv : pk.order) {
        const RowP* v = vk.find(kv.first);
        if (!v || (*v)->data != kv.second->data) return true;
    }
    return false;
}

struct Event {
    enum Kind { Merged, NewRow, Conflict, NameLost } kind = Merged;
    std::string table;
    std::int32_t id = 0;
    std::string mod;                  // the mod (the winner for a conflict)
    std::vector<std::string> cells;   // Conflict
    std::string loser;                // Conflict
};

using OwnerKey = std::tuple<std::string, std::int32_t, int, int>;  // table, ID, nth, cell index (-1 = row)
using Owners = std::map<OwnerKey, std::string>;

std::string owner_of(const Owners& m, const OwnerKey& k) {
    const auto it = m.find(k);
    return it == m.end() ? "?" : it->second;
}

// V = the original table, M = merged so far (edited in place), P = this mod's. The later mod wins.
// True when anything changed.
bool merge_table(const std::string& tname, const Param& V, Param& M, const Param& P, const Layout& layout, bool allow_vanilla_edits,
                 const std::string& label, Owners& owner, Owners& rowowner, std::vector<Event>& events) {
    const Keyed vk(V.rows);
    Keyed mk(M.rows);
    const Keyed pk(P.rows);
    std::vector<std::int32_t> ids;
    for (const RowP& r : M.rows) ids.push_back(r->id);
    const bool is_sorted = std::is_sorted(ids.begin(), ids.end());
    bool changed = false;
    for (const auto& kv : pk.order) {
        const Key& key = kv.first;
        const RowP& pr = kv.second;
        const RowP* vr = vk.find(key);
        const RowP* mr = mk.find(key);
        const OwnerKey rk{tname, key.first, key.second, -1};
        if (!vr) {
            if (!mr) {  // a row the original does not have
                auto nr = std::make_shared<Row>();
                nr->id = pr->id;
                nr->data = pr->data;
                if (pr->has_name) {
                    bool lost = false;
                    nr->name = convert_name(pr->name, P.unicode_names, M.unicode_names, lost);
                    nr->has_name = true;
                    if (lost) {
                        Event e;
                        e.kind = Event::NameLost;
                        e.table = tname;
                        e.id = pr->id;
                        e.mod = label;
                        events.push_back(std::move(e));
                    }
                }
                if (is_sorted) {  // a sorted table keeps its ID order
                    const std::size_t pos = static_cast<std::size_t>(std::upper_bound(ids.begin(), ids.end(), pr->id) - ids.begin());
                    M.rows.insert(M.rows.begin() + static_cast<std::ptrdiff_t>(pos), nr);
                    ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(pos), pr->id);
                } else {
                    M.rows.push_back(nr);
                    ids.push_back(pr->id);
                }
                mk.index[key] = nr;
                rowowner[rk] = label;
                Event e;
                e.kind = Event::NewRow;
                e.table = tname;
                e.id = pr->id;
                e.mod = label;
                events.push_back(std::move(e));
                changed = true;
            } else if ((*mr)->data != pr->data) {  // overlaps a new row another mod added first
                Bytes cur = (*mr)->data;
                std::vector<std::string> bad;
                for (const Cell& c : layout.cells)
                    if (cell_differs(pr->data, cur, c)) {
                        cell_copy(cur, pr->data, c);
                        bad.push_back(c.name);
                    }
                if (!bad.empty()) {
                    const std::string loser = owner_of(rowowner, rk);
                    (*mr)->data = std::move(cur);
                    rowowner[rk] = label;
                    Event e;
                    e.kind = Event::Conflict;
                    e.table = tname;
                    e.id = pr->id;
                    e.mod = label;
                    e.cells = std::move(bad);
                    e.loser = loser;
                    events.push_back(std::move(e));
                    changed = true;
                }
            }
            continue;
        }
        if (!allow_vanilla_edits || pr->data == (*vr)->data || !mr) continue;
        Bytes cur = (*mr)->data;
        bool touched = false;
        std::vector<std::string> bad;
        std::vector<std::string> losers;  // in order of first appearance
        for (std::size_t ci = 0; ci < layout.cells.size(); ++ci) {
            const Cell& c = layout.cells[ci];
            if (!cell_differs(pr->data, (*vr)->data, c)) continue;  // the mod left this cell as the original has it
            const OwnerKey ok{tname, key.first, key.second, static_cast<int>(ci)};
            if (!cell_differs(cur, (*vr)->data, c)) {  // nobody changed it yet
                cell_copy(cur, pr->data, c);
                owner[ok] = label;
                touched = true;
            } else if (cell_differs(cur, pr->data, c)) {  // both changed it, differently: the later mod wins
                const std::string l = owner_of(owner, ok);
                if (std::find(losers.begin(), losers.end(), l) == losers.end()) losers.push_back(l);
                bad.push_back(c.name);
                cell_copy(cur, pr->data, c);
                owner[ok] = label;
                touched = true;
            }
        }
        if (touched) {
            (*mr)->data = std::move(cur);
            changed = true;
            Event e;
            e.kind = Event::Merged;
            e.table = tname;
            e.id = pr->id;
            e.mod = label;
            events.push_back(std::move(e));
            for (const std::string& l : losers) {
                Event c;
                c.kind = Event::Conflict;
                c.table = tname;
                c.id = pr->id;
                c.mod = label;
                c.cells = bad;
                c.loser = l;
                events.push_back(std::move(c));
            }
        }
    }
    return changed;
}

// The merged table, written and read back, has to hold the same rows.
void verify(const Param& M) {
    const Param chk(M.write());
    if (chk.rows.size() != M.rows.size()) throw ParamError("self-check failed (row count)");
    for (std::size_t i = 0; i < M.rows.size(); ++i)
        if (chk.rows[i]->id != M.rows[i]->id || chk.rows[i]->data != M.rows[i]->data)
            throw ParamError("self-check failed (row " + std::to_string(M.rows[i]->id) + ")");
}


// An ordered map (insertion order kept, updating a key keeps its place).
template <typename V>
struct OrderedMap {
    std::vector<std::pair<std::string, V>> items;
    std::map<std::string, std::size_t> at;
    V* find(const std::string& k) {
        const auto it = at.find(k);
        return it == at.end() ? nullptr : &items[it->second].second;
    }
    V& operator[](const std::string& k) {
        const auto it = at.find(k);
        if (it != at.end()) return items[it->second].second;
        at[k] = items.size();
        items.emplace_back(k, V{});
        return items.back().second;
    }
};

struct CurEntry {
    gcn::Bnd4File file;
    bool original = true;  // still the original's entry
};

}  // namespace

bool param_merge(const Bytes& original, const Bytes& paramdef, const std::vector<ParamMod>& mods, const std::set<std::string>& allowed_tables,
                 bool dry, ParamReport& report, Bytes* out, std::string* error, const ParamLog& log, int compress_level) {
    report = ParamReport{};
    if (out) out->clear();
    auto say = [&](const std::string& s) {
        if (log) log(s);
    };
    auto warn = [&](const std::string& s) {
        say("  warning: " + s);
        report.warnings.push_back(s);
    };
    try {
        // The cell layouts, from the game's own paramdef.
        std::map<std::string, Layout> layouts;
        {
            gcn::Bnd4Archive D;
            std::string why;
            if (paramdef.empty() || !read_archive(paramdef, false, D, &why)) {
                if (error) *error = "paramdef: " + (paramdef.empty() ? std::string("missing") : why);
                return false;
            }
            for (const gcn::Bnd4File& f : D.files) {
                try {
                    Layout lay = read_paramdef(f.data);
                    layouts[lay.ptype] = std::move(lay);
                } catch (const std::exception& ex) {
                    warn("paramdef " + base_name(f) + " could not be read (" + ex.what() + ") - tables using it are compared by whole rows");
                }
            }
        }

        gcn::Bnd4Archive V;
        std::string why;
        if (!read_archive(original, true, V, &why)) {
            if (error) *error = "original: " + why;
            return false;
        }
        OrderedMap<const gcn::Bnd4File*> ventries;
        for (const gcn::Bnd4File& f : V.files) ventries[lower(base_name(f))] = &f;

        OrderedMap<CurEntry> cur_entries;  // merged so far, by table name
        for (const auto& kv : ventries.items) cur_entries[kv.first].file = *kv.second;
        std::map<std::string, std::unique_ptr<Param>> vparams, mparams;  // parse caches: original / being merged
        std::map<std::string, std::string> whole_owner;                   // tables a mod took as a whole
        std::map<std::string, std::vector<std::string>> table_mods;       // table -> the mods that really changed it
        Owners owner, rowowner;

        for (const ParamMod& mod : mods) {
            const std::string& label = mod.label;
            gcn::Bnd4Archive MB;
            if (!read_archive(mod.file, false, MB, &why)) {
                warn(label + ": the file could not be read, left out of the merge (" + why + ")");
                continue;
            }
            int ndiff = 0;
            struct Count {
                int modified = 0, added = 0;
            };
            OrderedMap<Count> counts;  // table -> rows
            struct Group {
                std::string table;
                std::int32_t id = 0;
                std::string winner, loser;
                std::vector<std::string> cells;
            };
            std::vector<Group> groups;
            for (const gcn::Bnd4File& me : MB.files) {
                const std::string base = base_name(me);
                const std::string key = lower(base);
                const gcn::Bnd4File* const* vep = ventries.find(key);
                if (!vep) {  // not in the original: added as it is
                    CurEntry& ce = cur_entries[key];
                    ce.file = me;
                    ce.original = false;
                    ++ndiff;
                    warn(label + ": table " + base + " is not in the original, added as a whole");
                    continue;
                }
                const gcn::Bnd4File& ve = **vep;
                if (me.data == ve.data) continue;
                const std::string tname = ends_with(lower(base), ".param") ? base.substr(0, base.size() - 6) : base;
                bool counted = false;
                std::unique_ptr<Param> P;
                try {  // a table that cannot be read is skipped: a damaged table never goes into the result
                    if (!vparams.count(key)) vparams[key] = std::make_unique<Param>(ve.data);
                    P = std::make_unique<Param>(me.data);
                } catch (const std::exception& ex) {
                    warn(label + ": table " + base + " cannot be read, this mod's version of it is left out (" + ex.what() + ")");
                    continue;
                }
                const Param& Vp = *vparams[key];
                try {
                    if (P->ptype == Vp.ptype && P->row_size == Vp.row_size && !semantic_diff(Keyed(Vp.rows), Keyed(P->rows)))
                        continue;  // same row data as the original (only names / file layout differ)
                    ++ndiff;
                    counted = true;
                    table_mods[key].push_back(label);
                    if (whole_owner.count(key))
                        throw ParamError("an earlier mod (" + whole_owner[key] + ") took this table as a whole, so it cannot be merged cell by cell");
                    if (P->ptype != Vp.ptype || P->row_size != Vp.row_size)
                        throw ParamError("row structure differs from the original (row size " + std::to_string(Vp.row_size) + " -> " + std::to_string(P->row_size) + ")");
                    if (!mparams.count(key)) mparams[key] = std::make_unique<Param>(ve.data);
                    Param& M = *mparams[key];
                    Layout layout;
                    const auto lit = layouts.find(Vp.ptype);
                    if (lit == layouts.end() || lit->second.row_size != Vp.row_size) {
                        layout = blob_layout(Vp.row_size);
                        warn(label + ": no matching paramdef for " + base + ", compared by whole rows");
                    } else {
                        layout = lit->second;
                    }
                    const std::string tkey = ends_with(lower(base), ".param") ? lower(base).substr(0, base.size() - 6) : lower(base);
                    const bool allow = allowed_tables.empty() || allowed_tables.count(tkey) > 0;
                    const Keyed vk_(Vp.rows), pk_(P->rows);
                    std::vector<std::int32_t> gone;
                    for (const auto& kv : vk_.order)
                        if (!pk_.find(kv.first)) gone.push_back(kv.first.first);
                    if (!gone.empty()) {  // the mod deleted original rows: deletions are not merged
                        std::string ids_text;
                        for (std::size_t i = 0; i < gone.size() && i < 5; ++i) ids_text += (i ? ", " : "") + std::to_string(gone[i]);
                        warn(label + ": " + base + " has " + std::to_string(gone.size()) +
                             " original rows deleted, but deleting rows is not merged and the original rows stay (ID " + ids_text +
                             (gone.size() > 5 ? " ..." : "") + ")");
                    }
                    if (!allow) {
                        bool edited = false;
                        for (const auto& kv : vk_.order) {
                            const RowP* pr = pk_.find(kv.first);
                            if (pr && (*pr)->data != kv.second->data) {
                                edited = true;
                                break;
                            }
                        }
                        // a table outside the list that a mod edited cannot be merged cell by cell: never dropped silently
                        if (edited) throw ParamError("not in the allowed table list, so it cannot be merged cell by cell");
                    }
                    std::vector<Event> events;
                    const bool changed = merge_table(tname, Vp, M, *P, layout, allow, label, owner, rowowner, events);
                    if (changed) verify(M);  // on failure the catch below drops this table and takes the mod's whole
                    for (const Event& ev : events) {
                        if (ev.kind == Event::Merged || ev.kind == Event::NewRow) {
                            Count& c = counts[ev.table];
                            ++(ev.kind == Event::Merged ? c.modified : c.added);
                        } else if (ev.kind == Event::Conflict) {
                            auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) {
                                return x.table == ev.table && x.id == ev.id && x.winner == ev.mod && x.loser == ev.loser;
                            });
                            if (g == groups.end()) {
                                groups.push_back({ev.table, ev.id, ev.mod, ev.loser, {}});
                                g = groups.end() - 1;
                            }
                            g->cells.insert(g->cells.end(), ev.cells.begin(), ev.cells.end());
                        } else {
                            warn(label + ": " + ev.table + " new row " + std::to_string(ev.id) +
                                 " has a name that cannot be converted to the table's encoding here; the name is left empty");
                        }
                    }
                } catch (const std::exception& ex) {
                    // One table cannot be read or its result does not check out: this table, and only it, is the mod's file as a whole (later mod wins).
                    ++report.table_fallbacks;
                    if (!counted) {
                        ++ndiff;
                        table_mods[key].push_back(label);
                    }
                    warn(label + ": merging " + base + " failed (" + ex.what() + ") -> the mod's table is taken as a whole");
                    mparams.erase(key);
                    CurEntry& ce = cur_entries[key];
                    ce.file = me;
                    ce.original = false;
                    whole_owner[key] = label;
                    std::vector<std::string> losers;
                    for (const std::string& x : table_mods[key])
                        if (x != label) losers.push_back(x);
                    if (!losers.empty()) {
                        ParamConflict cf;
                        cf.table = tname;
                        cf.winner = label;
                        cf.losers = losers;
                        report.conflicts.push_back(std::move(cf));
                    }
                }
            }
            report.diffs.emplace_back(label, ndiff);
            if (ndiff == 0)
                warn(label + " has no difference from the base file -> the base file may already be modified by this mod or may not be the original "
                             "(overlaps cannot be seen in that case)");
            for (const auto& kv : counts.items) {
                std::string bits;
                if (kv.second.modified) bits += "modified rows " + std::to_string(kv.second.modified);
                if (kv.second.added) bits += std::string(bits.empty() ? "" : ", ") + "new rows " + std::to_string(kv.second.added);
                say("  " + kv.first + " <- " + label + ": " + bits);
                report.merged_rows += kv.second.modified;
                report.new_rows += kv.second.added;
                report.changes.push_back({kv.first, label, kv.second.modified, kv.second.added});
            }
            for (Group& g : groups) {
                std::vector<std::string> cl;
                for (const std::string& c : g.cells)
                    if (std::find(cl.begin(), cl.end(), c) == cl.end()) cl.push_back(c);
                std::string txt;
                for (std::size_t i = 0; i < cl.size() && i < 8; ++i) txt += (i ? ", " : "") + cl[i];
                if (cl.size() > 8) txt += " and " + std::to_string(cl.size() - 8) + " more cells";
                report.conflict_cells += static_cast<int>(cl.size());
                say("  CONFLICT " + g.table + " ID " + std::to_string(g.id) + " cells " + txt + ": " + g.winner + " overwrites the change of " + g.loser);
                ParamConflict cf;
                cf.table = g.table;
                cf.id = g.id;
                cf.cells = std::move(cl);
                cf.winner = g.winner;
                cf.losers = {g.loser};
                report.conflicts.push_back(std::move(cf));
            }
        }

        // Assemble the result.
        gcn::Bnd4Archive res;
        res.head = V.head;
        res.entry_size = V.entry_size;
        res.unicode = V.unicode;
        int changed_tables = 0;
        for (const auto& kv : cur_entries.items) {
            const std::string& key = kv.first;
            const CurEntry& ce = kv.second;
            const auto mp = mparams.find(key);
            if (mp != mparams.end() && !whole_owner.count(key)) {
                gcn::Bnd4File ne = ce.file;
                ne.data = mp->second->write();
                if (ne.data != (*ventries.find(key))->data) ++changed_tables;
                res.files.push_back(std::move(ne));
            } else {
                if (!ce.original) ++changed_tables;
                res.files.push_back(ce.file);
            }
        }
        report.changed_tables = changed_tables;
        say("done: changed tables " + std::to_string(changed_tables) + ", rows merged cell by cell " + std::to_string(report.merged_rows) +
            ", new rows " + std::to_string(report.new_rows) + ", conflicting cells " + std::to_string(report.conflict_cells) +
            (report.table_fallbacks ? ", tables taken as a whole " + std::to_string(report.table_fallbacks) : std::string()));
        if (!dry && out) {
            const std::uint32_t n = static_cast<std::uint32_t>(res.files.size());
            std::memcpy(res.head.data() + 0xc, &n, 4);  // bnd4_pack keeps the header as given
            const Bytes packed = gcn::dcx_compress(gcn::bnd4_pack(res), compress_level);
            if (packed.empty()) {
                if (error) *error = "could not compress the result";
                return false;
            }
            *out = packed;
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

}  // namespace mods
