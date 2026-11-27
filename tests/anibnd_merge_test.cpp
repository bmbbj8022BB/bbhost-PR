// mods/anibnd_merge on archives built here, so it needs no game files: a TAE
// per animation set, wrapped as the game's anibnd (BND4 in a DCX).
//
// The merged TAE of two mods that changed different animations has to be the
// TAE built with both changes, byte for byte: the same layout, each
// animation's payload moved to where it belongs with its pointers adjusted.
#include "gcn/container.h"
#include "mods/anibnd_merge.h"

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

std::size_t align16(std::size_t x) { return (x + 15) & ~static_cast<std::size_t>(15); }

struct AnimSpec {
    std::int64_t id;
    int events, groups;
    int seed;
};

// One animation's payload at absolute file offset `start`: the record block,
// events and what they point at, event groups with their value arrays, and
// some time data, every pointer pointing inside the payload.
Bytes make_payload(std::size_t start, const AnimSpec& s, std::int64_t out[4]) {
    std::size_t pos = 0x20;
    const std::size_t eh = pos;
    pos += 24 * static_cast<std::size_t>(s.events);
    std::vector<std::size_t> evd;
    for (int j = 0; j < s.events; ++j) {
        evd.push_back(pos);
        pos += 16;
    }
    const std::size_t eg = pos;
    pos += 32 * static_cast<std::size_t>(s.groups);
    std::vector<std::size_t> vo;
    for (int g = 0; g < s.groups; ++g) {
        vo.push_back(pos);
        pos += 4 * static_cast<std::size_t>(g + 2);
    }
    const std::size_t et = pos;
    pos += 16;
    Bytes p(align16(pos));
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = static_cast<std::uint8_t>(s.seed * 31 + static_cast<int>(i) * 7);
    const auto abs = [&](std::size_t rel) { return static_cast<std::int64_t>(start + rel); };
    put<std::int64_t>(p, 0, s.seed);
    put<std::int64_t>(p, 8, abs(et));
    put<std::int64_t>(p, 16, abs(et) + 8);
    put<std::int64_t>(p, 24, s.seed * 3);
    for (int j = 0; j < s.events; ++j) {
        const std::size_t h = eh + 24 * static_cast<std::size_t>(j);
        put<std::int64_t>(p, h, abs(et));
        put<std::int64_t>(p, h + 8, abs(et) + 4);
        put<std::int64_t>(p, h + 16, abs(evd[static_cast<std::size_t>(j)]));
        put<std::int64_t>(p, evd[static_cast<std::size_t>(j)], s.seed * 100 + j);
        put<std::int64_t>(p, evd[static_cast<std::size_t>(j)] + 8, abs(et) + 8);
    }
    for (int g = 0; g < s.groups; ++g) {
        const std::size_t gp = eg + 32 * static_cast<std::size_t>(g);
        put<std::int64_t>(p, gp, g + 2);
        put<std::int64_t>(p, gp + 8, abs(vo[static_cast<std::size_t>(g)]));
        put<std::int64_t>(p, gp + 16, abs(et));
        put<std::int64_t>(p, gp + 24, 0);
        for (int t = 0; t < g + 2; ++t) put<std::int32_t>(p, vo[static_cast<std::size_t>(g)] + 4 * static_cast<std::size_t>(t), static_cast<std::int32_t>(abs(et)) + t);
    }
    out[0] = abs(eh);
    out[1] = abs(eg);
    out[2] = abs(et);
    out[3] = abs(0);
    return p;
}

Bytes make_tae(const std::vector<AnimSpec>& anims) {
    const std::size_t cnt = anims.size(), ao = 0x80, first = align16(ao + 16 * cnt), struct_end = first + cnt * 0x30;
    Bytes b(struct_end);
    std::memcpy(b.data(), "TAE ", 4);
    put<std::uint32_t>(b, 8, 0x1000C);
    put<std::int32_t>(b, 0x54, static_cast<std::int32_t>(cnt));
    put<std::int64_t>(b, 0x58, ao);
    put<std::int64_t>(b, 0x78, first);
    std::size_t pos = struct_end;
    for (std::size_t i = 0; i < cnt; ++i) {
        std::int64_t ptr[4];
        const Bytes pl = make_payload(pos, anims[i], ptr);
        const std::size_t off = first + i * 0x30;
        put<std::int64_t>(b, ao + 16 * i, anims[i].id);
        put<std::int64_t>(b, ao + 16 * i + 8, static_cast<std::int64_t>(off));
        for (int k = 0; k < 4; ++k) put<std::int64_t>(b, off + 8 * static_cast<std::size_t>(k), ptr[k]);
        put<std::int32_t>(b, off + 32, anims[i].events);
        put<std::int32_t>(b, off + 36, anims[i].groups);
        put<std::int32_t>(b, off + 40, 1);
        b.insert(b.end(), pl.begin(), pl.end());
        pos += pl.size();
    }
    put<std::int32_t>(b, 0xC, static_cast<std::int32_t>(b.size()));
    return b;
}

using Entries = std::vector<std::pair<std::string, Bytes>>;

Bytes utf16(const std::string& s) {
    Bytes n;
    for (char c : s) {
        n.push_back(static_cast<std::uint8_t>(c));
        n.push_back(0);
    }
    return n;
}

// The game's anibnd layout: BND4, UTF-16 names, 0x24-byte entries, in a DCX.
Bytes make_anibnd(const Entries& entries, bool dcx = true) {
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
    std::uint32_t id = 0;
    for (const auto& e : entries) {
        gcn::Bnd4File f;
        f.flags = 0x40;
        f.id = id++;
        f.name = utf16(e.first);
        f.data = e.second;
        a.files.push_back(std::move(f));
    }
    const Bytes raw = gcn::bnd4_pack(a);
    return dcx ? gcn::dcx_compress(raw) : raw;
}

std::map<std::string, Bytes> entries_of(const Bytes& dcx) {
    std::map<std::string, Bytes> m;
    gcn::Bnd4Archive a;
    if (!gcn::bnd4_read(gcn::dcx_decompress(dcx), a)) return m;
    for (const auto& f : a.files) {
        std::string n;
        for (std::size_t i = 0; i + 1 < f.name.size(); i += 2) n.push_back(static_cast<char>(f.name[i]));
        m[n] = f.data;
    }
    return m;
}

const char* kA000 = "chr\\c0000\\tae\\a000.tae";
const char* kA001 = "chr\\c0000\\tae\\a001.tae";
const char* kBeh = "chr\\c0000\\hkx\\Behaviors\\c0000.hkx";
const char* kSkel = "chr\\c0000\\hkx\\Skeletons\\c0000.hkx";

// An archive like the original, with some entries replaced.
Entries with(const Entries& base, const Entries& changes) {
    Entries out;
    for (const auto& e : base) {
        Bytes data = e.second;
        for (const auto& c : changes)
            if (c.first == e.first) data = c.second;
        out.emplace_back(e.first, std::move(data));
    }
    for (const auto& c : changes) {
        bool have = false;
        for (const auto& e : base) have = have || e.first == c.first;
        if (!have) out.push_back(c);
    }
    return out;
}

Bytes bytes_of(const char* s) { return Bytes(s, s + std::strlen(s)); }

const std::vector<AnimSpec> kVanilla = {{3000, 2, 1, 1}, {3001, 1, 2, 2}, {3002, 3, 1, 3}, {3000, 1, 1, 4}};

bool merge(const Bytes& original, const std::vector<mods::AnibndMod>& mm, mods::AnibndReport& rep, Bytes* out, std::string* err = nullptr, bool dry = false) {
    std::string e;
    const bool ok = mods::anibnd_merge(original, mm, dry, rep, out, &e);
    if (err) *err = e;
    return ok;
}

}  // namespace

int main() {
    const Entries vanilla = {{kA000, make_tae(kVanilla)},
                             {kA001, make_tae({{100, 1, 1, 10}, {101, 2, 2, 11}})},
                             {kBeh, bytes_of("behavior graph of the original")},
                             {kSkel, bytes_of("skeleton")}};
    const Bytes original = make_anibnd(vanilla);

    // Different animations of the same TAE: both are taken, nothing conflicts,
    // and the file is the one built with both changes.
    {
        auto a_tae = kVanilla;
        a_tae[0] = {3000, 2, 1, 101};  // A: animation 3000 changed
        auto b_tae = kVanilla;
        b_tae[1] = {3001, 3, 1, 202};  // B: animation 3001 changed, a different size
        const Bytes A = make_anibnd(with(vanilla, {{kA000, make_tae(a_tae)}}));
        const Bytes B = make_anibnd(with(vanilla, {{kA000, make_tae(b_tae)}}));
        mods::AnibndReport rep;
        Bytes out;
        CHECK(merge(original, {{"A", A}, {"B", B}}, rep, &out));
        CHECK(rep.conflicts.empty());
        CHECK(rep.entries_merged == 1);
        auto both = kVanilla;
        both[0] = a_tae[0];
        both[1] = b_tae[1];
        const auto m = entries_of(out);
        CHECK(m.size() == 4);
        CHECK(m.count(kA000) && m.at(kA000) == make_tae(both));
        CHECK(m.count(kA001) && m.at(kA001) == vanilla[1].second);  // untouched entries stay as they were
        CHECK(m.count(kBeh) && m.at(kBeh) == vanilla[2].second);
        // A dry run builds and checks the same but writes nothing.
        mods::AnibndReport dry_rep;
        Bytes dry_out;
        CHECK(merge(original, {{"A", A}, {"B", B}}, dry_rep, &dry_out, nullptr, true));
        CHECK(dry_out.empty() && dry_rep.entries_merged == 1);
    }

    // The same animation changed by both: the later mod wins and the report
    // says who lost. The same id occurring twice is two animations.
    {
        auto a_tae = kVanilla;
        a_tae[0] = {3000, 2, 1, 101};
        a_tae[3] = {3000, 1, 1, 104};  // the second 3000
        auto b_tae = kVanilla;
        b_tae[0] = {3000, 2, 2, 201};
        const Bytes A = make_anibnd(with(vanilla, {{kA000, make_tae(a_tae)}}));
        const Bytes B = make_anibnd(with(vanilla, {{kA000, make_tae(b_tae)}}));
        mods::AnibndReport rep;
        Bytes out;
        CHECK(merge(original, {{"A", A}, {"B", B}}, rep, &out));
        CHECK(rep.conflicts.size() == 1);
        if (rep.conflicts.size() == 1) {
            const auto& c = rep.conflicts[0];
            CHECK(c.entry == "a000.tae" && c.anim == 3000 && c.index == 0);
            CHECK(c.winner == "B" && c.losers == std::vector<std::string>{"A"});
        }
        auto want = kVanilla;
        want[0] = b_tae[0];  // B wins 3000, A's second 3000 is kept
        want[3] = a_tae[3];
        CHECK(entries_of(out).at(kA000) == make_tae(want));
        // The other way round, A wins.
        mods::AnibndReport rep2;
        Bytes out2;
        CHECK(merge(original, {{"B", B}, {"A", A}}, rep2, &out2));
        auto want2 = kVanilla;
        want2[0] = a_tae[0];
        want2[3] = a_tae[3];
        CHECK(entries_of(out2).at(kA000) == make_tae(want2));
    }

    // Behavior graphs and other entries cannot be merged: one mod's is taken,
    // two mods' is a conflict, and the same change from two mods is not one.
    {
        const Bytes A = make_anibnd(with(vanilla, {{kBeh, bytes_of("graph of A")}}));
        const Bytes B = make_anibnd(with(vanilla, {{kBeh, bytes_of("graph of B")}}));
        const Bytes A2 = make_anibnd(with(vanilla, {{kBeh, bytes_of("graph of A")}}));
        const Bytes none = make_anibnd(vanilla);
        mods::AnibndReport rep;
        Bytes out;
        CHECK(merge(original, {{"A", A}, {"none", none}}, rep, &out));
        CHECK(rep.conflicts.empty() && rep.taken.size() == 1 && rep.taken[0].entry == "Behaviors/c0000.hkx");
        CHECK(entries_of(out).at(kBeh) == bytes_of("graph of A"));
        CHECK(rep.diffs.size() == 2 && rep.diffs[1].second == 0 && !rep.warnings.empty());  // "none" differs in nothing
        CHECK(merge(original, {{"A", A}, {"B", B}}, rep, &out));
        CHECK(rep.conflicts.size() == 1 && rep.conflicts[0].anim == -1 && rep.conflicts[0].winner == "B");
        CHECK(rep.conflicts[0].entry == "Behaviors/c0000.hkx");
        CHECK(entries_of(out).at(kBeh) == bytes_of("graph of B"));
        CHECK(merge(original, {{"A", A}, {"A2", A2}}, rep, &out));
        CHECK(rep.conflicts.empty());
    }

    // An entry a mod adds is in the result; a mod may also be a bare BND4.
    {
        const Bytes extra = make_tae({{9, 1, 1, 9}});
        const Bytes X = make_anibnd(with(vanilla, {{"chr\\c0000\\tae\\a900.tae", extra}}), false);
        mods::AnibndReport rep;
        Bytes out;
        CHECK(merge(original, {{"X", X}}, rep, &out));
        const auto m = entries_of(out);
        CHECK(m.size() == 5 && m.count("chr\\c0000\\tae\\a900.tae") && m.at("chr\\c0000\\tae\\a900.tae") == extra);
    }

    // Mods that add different new animations cannot share a layout: one
    // mod's are kept and the report says some were left out.
    {
        auto x = kVanilla;
        x.push_back({4000, 1, 1, 5});
        auto y = kVanilla;
        y.push_back({4001, 2, 1, 6});
        const Bytes X = make_anibnd(with(vanilla, {{kA000, make_tae(x)}}));
        const Bytes Y = make_anibnd(with(vanilla, {{kA000, make_tae(y)}}));
        mods::AnibndReport rep;
        Bytes out;
        CHECK(merge(original, {{"X", X}, {"Y", Y}}, rep, &out));
        CHECK(!rep.warnings.empty());
        CHECK(entries_of(out).at(kA000) == make_tae(x));
    }

    // Inputs that cannot be read are refused with a message, not merged.
    {
        mods::AnibndReport rep;
        Bytes out;
        std::string err;
        CHECK(!merge(make_anibnd(vanilla, false), {}, rep, &out, &err));  // the original must be a DCX
        CHECK(!err.empty());
        CHECK(!merge(original, {{"broken", bytes_of("not an archive at all")}}, rep, &out, &err));
        CHECK(err.find("broken") != std::string::npos);
        CHECK(out.empty());
    }

    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("anibnd_merge: ok\n");
    return 0;
}
