// mods/param_merge on archives built here, so it needs no game files: a
// paramdef (one definition with plain cells and bit fields), and
// gameparam-like archives with a few small PARAM tables (BND4 in a DCX).
#include "gcn/container.h"
#include "mods/param_merge.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

int g_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failed;                                                      \
        }                                                                    \
    } while (0)

template <typename T>
void put(Bytes& b, std::size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(T));
}
template <typename T>
T get(const Bytes& b, std::size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

Bytes utf16(const std::string& s) {
    Bytes b;
    for (char c : s) {
        b.push_back(static_cast<std::uint8_t>(c));
        b.push_back(0);
    }
    return b;
}

gcn::Bnd4Archive new_archive(const std::vector<std::pair<std::string, Bytes>>& entries) {
    gcn::Bnd4Archive a;
    a.head.assign(0x40, 0);
    std::memcpy(a.head.data(), "BND4", 4);
    a.head[0x30] = 1;
    a.head[0x31] = 0x74;
    put<std::int64_t>(a.head, 0x10, 0x40);
    put<std::int64_t>(a.head, 0x20, 0x24);
    put<std::int32_t>(a.head, 0xC, static_cast<std::int32_t>(entries.size()));
    a.entry_size = 0x24;
    a.unicode = true;
    int id = 0;
    for (const auto& e : entries) {
        gcn::Bnd4File f;
        f.flags = 0x40;
        f.id = id++;
        f.name = utf16("N:\\param\\gameparam\\" + e.first);
        f.data = e.second;
        a.files.push_back(std::move(f));
    }
    return a;
}

Bytes dcx_of(const std::vector<std::pair<std::string, Bytes>>& entries) { return gcn::dcx_compress(gcn::bnd4_pack(new_archive(entries))); }

// ---- the paramdef: TEST_PARAM_ST
//   u8 a (0)  u8 b (1)  s32 c (2..5)  u8 f0:1  u8 f1:3 (both in the byte at 6)  dummy8 pad[3] (7..9)  f32 d (10..13)  -> 14 bytes
constexpr int kRow = 14;

Bytes paramdef_entry(const std::string& ptype, const std::vector<std::pair<std::string, std::string>>& fields) {
    const std::size_t esz = 0xD0;
    Bytes d(0x38 + fields.size() * esz, 0);
    put<std::int32_t>(d, 0, static_cast<std::int32_t>(d.size()));
    put<std::uint16_t>(d, 4, 0xFF);
    put<std::uint16_t>(d, 6, 1);
    put<std::uint16_t>(d, 8, static_cast<std::uint16_t>(fields.size()));
    put<std::uint16_t>(d, 0xA, static_cast<std::uint16_t>(esz));
    std::memcpy(d.data() + 0xC, ptype.data(), ptype.size());
    d[0x2D] = 1;
    put<std::uint16_t>(d, 0x2E, 201);
    put<std::int64_t>(d, 0x30, 0x38);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::size_t o = 0x38 + i * esz;
        std::memcpy(d.data() + o + 0x40, fields[i].first.data(), fields[i].first.size());
        std::memcpy(d.data() + o + 0x90, fields[i].second.data(), fields[i].second.size());
    }
    return d;
}

Bytes test_paramdef(bool broken_type = false) {
    return dcx_of({{"TEST_PARAM_ST.paramdef",
                    paramdef_entry("TEST_PARAM_ST", {{"u8", "a"}, {"u8", "b"}, {"s32", "c"}, {"u8", "f0:1"}, {"u8", "f1:3"},
                                                     {"dummy8", "pad[3]"}, {broken_type ? "wat" : "f32", "d"}})}});
}

// ---- PARAM tables
struct TRow {
    std::int32_t id;
    Bytes data;
    std::string name;
    bool has_name = true;
};

Bytes row_data(std::uint8_t a, std::uint8_t b, std::int32_t c, std::uint8_t bits, float d) {
    Bytes r(kRow, 0);
    r[0] = a;
    r[1] = b;
    put<std::int32_t>(r, 2, c);
    r[6] = bits;
    put<float>(r, 10, d);
    return r;
}

Bytes param_file(const std::string& ptype, const std::vector<TRow>& rows, std::size_t row_size = kRow) {
    const std::size_t n = rows.size();
    const std::size_t data_start = 0x40 + 24 * n, strings_off = data_start + row_size * n;
    Bytes names, f(strings_off, 0);
    put<std::int32_t>(f, 0, static_cast<std::int32_t>(strings_off));
    put<std::uint16_t>(f, 0x0A, static_cast<std::uint16_t>(n));
    std::memcpy(f.data() + 0x0C, ptype.data(), ptype.size());
    f[0x2E] = 1;  // unicode names
    put<std::int64_t>(f, 0x30, static_cast<std::int64_t>(data_start));
    for (std::size_t i = 0; i < n; ++i) {
        std::int64_t noff = 0;
        if (rows[i].has_name) {
            noff = static_cast<std::int64_t>(strings_off + names.size());
            const Bytes u = utf16(rows[i].name);
            names.insert(names.end(), u.begin(), u.end());
            names.push_back(0);
            names.push_back(0);
        }
        put<std::int32_t>(f, 0x40 + 24 * i, rows[i].id);
        put<std::int64_t>(f, 0x40 + 24 * i + 8, static_cast<std::int64_t>(data_start + i * row_size));
        put<std::int64_t>(f, 0x40 + 24 * i + 16, noff);
        std::memcpy(f.data() + data_start + i * row_size, rows[i].data.data(), std::min(row_size, rows[i].data.size()));
    }
    f.insert(f.end(), names.begin(), names.end());
    return f;
}

// What a merged archive holds for one table: ID -> row data (a repeated ID keeps the last).
std::map<std::int32_t, Bytes> table_rows(const Bytes& dcx, const std::string& table) {
    std::map<std::int32_t, Bytes> out;
    gcn::Bnd4Archive a;
    if (!gcn::bnd4_read(gcn::dcx_decompress(dcx), a)) return out;
    for (const auto& f : a.files) {
        std::string n;
        for (std::size_t i = 0; i + 1 < f.name.size(); i += 2) n.push_back(static_cast<char>(f.name[i]));
        if (n.size() < table.size() || n.compare(n.size() - table.size(), table.size(), table) != 0) continue;
        const std::int32_t cnt = static_cast<std::int32_t>(get<std::uint16_t>(f.data, 0x0A));
        const std::int64_t ds = get<std::int64_t>(f.data, 0x30), so = get<std::int32_t>(f.data, 0);
        const std::int64_t rs = cnt ? (so - ds) / cnt : 0;
        for (std::int32_t i = 0; i < cnt; ++i) {
            const std::int32_t id = get<std::int32_t>(f.data, 0x40 + 24 * static_cast<std::size_t>(i));
            out[id] = Bytes(f.data.begin() + ds + i * rs, f.data.begin() + ds + (i + 1) * rs);
        }
    }
    return out;
}

std::vector<std::int32_t> table_ids(const Bytes& dcx, const std::string& table) {
    std::vector<std::int32_t> ids;
    gcn::Bnd4Archive a;
    if (!gcn::bnd4_read(gcn::dcx_decompress(dcx), a)) return ids;
    for (const auto& f : a.files) {
        std::string n;
        for (std::size_t i = 0; i + 1 < f.name.size(); i += 2) n.push_back(static_cast<char>(f.name[i]));
        if (n.size() < table.size() || n.compare(n.size() - table.size(), table.size(), table) != 0) continue;
        const std::int32_t cnt = static_cast<std::int32_t>(get<std::uint16_t>(f.data, 0x0A));
        for (std::int32_t i = 0; i < cnt; ++i) ids.push_back(get<std::int32_t>(f.data, 0x40 + 24 * static_cast<std::size_t>(i)));
    }
    return ids;
}

struct Result {
    bool ok = false;
    Bytes out;
    mods::ParamReport rep;
    std::string error;
};

Result run(const Bytes& orig, const Bytes& pdef, const std::vector<mods::ParamMod>& m, const std::set<std::string>& allowed = {}, bool dry = false) {
    Result r;
    r.ok = mods::param_merge(orig, pdef, m, allowed, dry, r.rep, &r.out, &r.error);
    return r;
}

// The original: tables T1 (sorted IDs 10 20 30 40) and T2 (one row).
struct Base {
    std::vector<TRow> t1, t2;
    Bytes dcx;
};
Base make_base() {
    Base b;
    b.t1 = {{10, row_data(1, 2, 100, 0, 1.0f), "ten"}, {20, row_data(3, 4, 200, 0, 2.0f), "twenty"},
            {30, row_data(5, 6, 300, 0, 3.0f), "thirty"}, {40, row_data(7, 8, 400, 0, 4.0f), "forty"}};
    b.t2 = {{1, row_data(9, 9, 9, 0, 9.0f), "one"}};
    b.dcx = dcx_of({{"T1.param", param_file("TEST_PARAM_ST", b.t1)}, {"T2.param", param_file("TEST_PARAM_ST", b.t2)}});
    return b;
}
Bytes mod_of(const Base& b, std::vector<TRow> t1, const std::vector<TRow>* t2 = nullptr, std::size_t rs1 = kRow) {
    return dcx_of({{"T1.param", param_file("TEST_PARAM_ST", t1, rs1)}, {"T2.param", param_file("TEST_PARAM_ST", t2 ? *t2 : b.t2)}});
}

void test_different_cells_merge() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1, tb = b.t1;
    ta[1].data[0] = 33;                      // A: row 20, cell a
    tb[1].data[1] = 44;                      // B: row 20, cell b (the same row, another cell)
    put<std::int32_t>(tb[2].data, 2, 3333);  // B: row 30, cell c
    const Result r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"B", mod_of(b, tb)}});
    CHECK(r.ok);
    CHECK(r.rep.conflicts.empty());
    CHECK(r.rep.changed_tables == 1);
    CHECK(r.rep.merged_rows == 3);  // A: row 20; B: rows 20 and 30
    const auto rows = table_rows(r.out, "T1.param");
    CHECK(rows.at(20)[0] == 33 && rows.at(20)[1] == 44);
    CHECK(get<std::int32_t>(rows.at(30), 2) == 3333);
    CHECK(rows.at(10) == b.t1[0].data && rows.at(40) == b.t1[3].data);  // untouched rows stay
    CHECK(table_rows(r.out, "T2.param").at(1) == b.t2[0].data);        // an untouched table stays
}

void test_same_cell_later_wins() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1, tb = b.t1;
    ta[0].data[0] = 11;
    tb[0].data[0] = 22;
    const Result r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"B", mod_of(b, tb)}});
    CHECK(r.ok && r.rep.conflicts.size() == 1);
    CHECK(table_rows(r.out, "T1.param").at(10)[0] == 22);
    if (r.rep.conflicts.size() == 1) {
        const auto& c = r.rep.conflicts[0];
        CHECK(c.table == "T1" && c.id == 10 && c.winner == "B" && c.losers.size() == 1 && c.losers[0] == "A");
        CHECK(c.cells.size() == 1 && c.cells[0] == "a");
    }
    CHECK(r.rep.conflict_cells == 1);
    // reversed: the other one wins
    const Result q = run(b.dcx, pdef, {{"B", mod_of(b, tb)}, {"A", mod_of(b, ta)}});
    CHECK(q.ok && table_rows(q.out, "T1.param").at(10)[0] == 11);
    // the same value from both is not a conflict
    const Result s = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"A2", mod_of(b, ta)}});
    CHECK(s.ok && s.rep.conflicts.empty() && table_rows(s.out, "T1.param").at(10)[0] == 11);
}

void test_bit_fields() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1, tb = b.t1, tc = b.t1;
    ta[0].data[6] = 0x01;  // f0 = 1
    tb[0].data[6] = 0x06;  // f1 = 3 (bits 1-3)
    tc[0].data[6] = 0x04;  // f1 = 2
    const Result r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"B", mod_of(b, tb)}});
    CHECK(r.ok && r.rep.conflicts.empty());
    CHECK(table_rows(r.out, "T1.param").at(10)[6] == 0x07);  // both bits kept
    const Result q = run(b.dcx, pdef, {{"B", mod_of(b, tb)}, {"C", mod_of(b, tc)}});
    CHECK(q.ok && q.rep.conflicts.size() == 1 && q.rep.conflicts[0].cells.size() == 1 && q.rep.conflicts[0].cells[0] == "f1");
    CHECK(table_rows(q.out, "T1.param").at(10)[6] == 0x04);
}

void test_new_rows() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1, tb = b.t1;
    ta.insert(ta.begin() + 2, {25, row_data(1, 1, 1, 0, 1.5f), "twentyfive"});  // sorted position
    tb.insert(tb.begin() + 2, {25, row_data(2, 2, 2, 0, 2.5f), "twentyfive-b"});
    tb.push_back({50, row_data(5, 5, 5, 0, 5.5f), "fifty"});
    const Result r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"B", mod_of(b, tb)}});
    CHECK(r.ok);
    const std::vector<std::int32_t> want = {10, 20, 25, 30, 40, 50};
    CHECK(table_ids(r.out, "T1.param") == want);
    CHECK(r.rep.new_rows == 2);  // 25 (A) and 50 (B); B's 25 lands on A's new row, which is a conflict, not a new row
    CHECK(r.rep.conflicts.size() == 1 && r.rep.conflicts[0].id == 25 && r.rep.conflicts[0].winner == "B");
    CHECK(table_rows(r.out, "T1.param").at(25)[0] == 2);
}

void test_whole_table_fallbacks() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1;
    ta[0].data[0] = 11;
    // a mod whose rows are bigger: that table cannot be merged cell by cell -> taken as a whole
    auto wide = b.t1;
    for (auto& r : wide) r.data.resize(kRow + 4, 7);
    const Bytes mw = mod_of(b, wide, nullptr, kRow + 4);
    const Result r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"W", mw}});
    CHECK(r.ok && r.rep.table_fallbacks == 1 && !r.rep.warnings.empty());
    CHECK(r.rep.conflicts.size() == 1 && r.rep.conflicts[0].id == -1 && r.rep.conflicts[0].winner == "W" && r.rep.conflicts[0].losers[0] == "A");
    CHECK(table_rows(r.out, "T1.param").at(10).size() == kRow + 4);
    // a damaged table is left out, never put into the result
    gcn::Bnd4Archive a = new_archive({{"T1.param", param_file("TEST_PARAM_ST", b.t1)}, {"T2.param", param_file("TEST_PARAM_ST", b.t2)}});
    a.files[0].data.resize(100);
    const Result d = run(b.dcx, pdef, {{"D", gcn::dcx_compress(gcn::bnd4_pack(a))}});
    CHECK(d.ok && !d.rep.warnings.empty() && d.rep.changed_tables == 0);
    CHECK(table_rows(d.out, "T1.param").at(10) == b.t1[0].data);
    // a file that is no archive at all is left out
    const Result g = run(b.dcx, pdef, {{"bad", Bytes{'n', 'o', 'p', 'e', 0, 0, 0, 0}}, {"A", mod_of(b, ta)}});
    CHECK(g.ok && g.rep.warnings.size() == 1 && table_rows(g.out, "T1.param").at(10)[0] == 11);
}

void test_allowed_tables() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1, tb = b.t1;
    ta[0].data[0] = 11;
    tb[1].data[0] = 22;
    // only T2 may have original rows edited cell by cell; T1 edits by two mods cannot be merged: the later mod takes the whole table
    const Result r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}, {"B", mod_of(b, tb)}}, {"t2"});
    CHECK(r.ok && r.rep.table_fallbacks == 2);
    const auto rows = table_rows(r.out, "T1.param");
    CHECK(rows.at(10)[0] == b.t1[0].data[0] && rows.at(20)[0] == 22);  // B's table as it is
    // new rows alone are fine in a table outside the list
    auto tn = b.t1;
    tn.push_back({60, row_data(6, 6, 6, 0, 6.0f), "sixty"});
    const Result n = run(b.dcx, pdef, {{"N", mod_of(b, tn)}}, {"t2"});
    CHECK(n.ok && n.rep.table_fallbacks == 0 && n.rep.new_rows == 1);
}

void test_inputs_and_dry() {
    const Base b = make_base();
    const Bytes pdef = test_paramdef();
    auto ta = b.t1;
    ta[0].data[0] = 11;
    Result r = run(b.dcx, Bytes{}, {{"A", mod_of(b, ta)}});
    CHECK(!r.ok && !r.error.empty());                       // no paramdef
    r = run(gcn::bnd4_pack(new_archive({{"T1.param", Bytes(0x80, 0)}})), pdef, {});
    CHECK(!r.ok && !r.error.empty());                       // the original must be a DCX
    r = run(b.dcx, test_paramdef(true), {{"A", mod_of(b, ta)}});
    CHECK(r.ok && !r.rep.warnings.empty());                 // an unreadable definition: compared by whole rows, still merged
    CHECK(table_rows(r.out, "T1.param").at(10)[0] == 11);
    r = run(b.dcx, pdef, {{"A", mod_of(b, ta)}}, {}, true);
    CHECK(r.ok && r.out.empty() && r.rep.merged_rows == 1);  // dry: built and counted, not packed
    r = run(b.dcx, pdef, {{"A", mod_of(b, b.t1)}});          // a mod equal to the original
    CHECK(r.ok && r.rep.diffs.size() == 1 && r.rep.diffs[0].second == 0 && r.rep.changed_tables == 0);
}

}  // namespace

int main() {
    test_different_cells_merge();
    test_same_cell_later_wins();
    test_bit_fields();
    test_new_rows();
    test_whole_table_fallbacks();
    test_allowed_tables();
    test_inputs_and_dry();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("param_merge_test: all passed\n");
    return 0;
}
