#include "mods/anibnd_merge.h"

#include "gcn/container.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace mods {
namespace {

// ---- reading and writing little-endian fields, checked ------------------

struct TaeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <typename T>
T rd(const std::vector<std::uint8_t>& b, std::int64_t at) {
    if (at < 0 || static_cast<std::uint64_t>(at) + sizeof(T) > b.size()) throw TaeError("a field lies outside the file");
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

template <typename T>
void wr(std::vector<std::uint8_t>& b, std::int64_t at, T v) {
    if (at < 0 || static_cast<std::uint64_t>(at) + sizeof(T) > b.size()) throw TaeError("a field lies outside the file");
    std::memcpy(b.data() + at, &v, sizeof(T));
}

// ---- TAE (Bloodborne / DS3, version 0x1000C), animation by animation -----
//
// One animation's payload (its events, event groups and the data they point
// at) is a self-contained block whose pointers all point into itself, so it
// can be swapped whole into another file of the same layout. An animation is
// keyed by (id, n-th time that id occurs): some mods carry an id twice.

using AnimKey = std::pair<std::int64_t, int>;

struct Slot {
    std::int64_t pos;  // in the payload
    bool wide;         // 8-byte pointer; else 4
};

struct Anim {
    AnimKey key;
    std::int32_t ec = 0, gc = 0, tc = 0;
    std::vector<std::uint8_t> payload;
    std::int64_t start = 0;  // where the payload began in its own file
    std::vector<Slot> slots;
    std::int64_t rel[4] = {0, 0, 0, 0};  // the four block offsets, from the payload start
    std::int64_t struct_off = 0;         // the 0x30 byte record in the header
};

// An animation with every pointer made relative to its payload, so two
// animations compare equal wherever they sit.
struct Canon {
    std::int32_t ec, gc, tc;
    std::int64_t rel[4];
    std::vector<std::uint8_t> bytes;
    bool operator==(const Canon& o) const {
        return ec == o.ec && gc == o.gc && tc == o.tc && std::equal(rel, rel + 4, o.rel) && bytes == o.bytes;
    }
    bool operator!=(const Canon& o) const { return !(*this == o); }
};

struct Tae {
    std::vector<std::uint8_t> head;
    std::int32_t cnt = 0;
    std::int64_t ao = 0, first = 0, struct_end = 0;
    std::map<AnimKey, Anim> anims;
    std::vector<AnimKey> order;  // by payload position

    explicit Tae(const std::vector<std::uint8_t>& b) {
        if (b.size() < 0x80 || std::memcmp(b.data(), "TAE ", 4) != 0 || rd<std::uint32_t>(b, 8) != 0x1000C)
            throw TaeError("not a TAE this reads");
        cnt = rd<std::int32_t>(b, 0x54);
        ao = rd<std::int64_t>(b, 0x58);
        first = rd<std::int64_t>(b, 0x78);
        if (cnt < 0) throw TaeError("a negative animation count");
        struct Rec {
            std::int64_t aid, off, eh, eg, et, af;
            std::int32_t ec, gc, tc;
            int occ;
        };
        std::vector<Rec> recs;
        std::map<std::int64_t, int> occ;
        for (std::int32_t i = 0; i < cnt; ++i) {
            Rec r;
            r.aid = rd<std::int64_t>(b, ao + 16 * i);
            r.off = rd<std::int64_t>(b, ao + 16 * i + 8);
            r.eh = rd<std::int64_t>(b, r.off);
            r.eg = rd<std::int64_t>(b, r.off + 8);
            r.et = rd<std::int64_t>(b, r.off + 16);
            r.af = rd<std::int64_t>(b, r.off + 24);
            r.ec = rd<std::int32_t>(b, r.off + 32);
            r.gc = rd<std::int32_t>(b, r.off + 36);
            r.tc = rd<std::int32_t>(b, r.off + 40);
            r.occ = occ[r.aid]++;
            recs.push_back(r);
        }
        struct_end = first + static_cast<std::int64_t>(cnt) * 0x30;
        if (struct_end < 0 || static_cast<std::uint64_t>(struct_end) > b.size()) throw TaeError("the header lies outside the file");
        std::vector<int> idx(static_cast<std::size_t>(cnt));
        for (int i = 0; i < cnt; ++i) idx[static_cast<std::size_t>(i)] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) { return recs[static_cast<std::size_t>(x)].af < recs[static_cast<std::size_t>(y)].af; });
        std::vector<std::int64_t> starts;
        for (int i : idx) starts.push_back(recs[static_cast<std::size_t>(i)].af);
        if (cnt && starts[0] != struct_end) throw TaeError("the first payload does not follow the header");
        if (std::set<std::int64_t>(starts.begin(), starts.end()).size() != starts.size()) throw TaeError("payloads overlap");
        std::vector<std::int64_t> ends(starts.begin() + (cnt ? 1 : 0), starts.end());
        ends.push_back(static_cast<std::int64_t>(b.size()));
        head.assign(b.begin(), b.begin() + struct_end);
        for (std::size_t k = 0; k < idx.size(); ++k) {
            const Rec& r = recs[static_cast<std::size_t>(idx[k])];
            const std::int64_t s = starts[k], e = ends[k];
            if (s > e || static_cast<std::uint64_t>(e) > b.size()) throw TaeError("a payload lies outside the file");
            Anim a;
            a.key = {r.aid, r.occ};
            a.ec = r.ec;
            a.gc = r.gc;
            a.tc = r.tc;
            a.start = s;
            a.payload.assign(b.begin() + s, b.begin() + e);
            a.struct_off = r.off;
            a.slots.push_back({r.af + 8 - s, true});
            a.slots.push_back({r.af + 0x10 - s, true});
            for (std::int32_t j = 0; j < r.ec; ++j) {
                const std::int64_t h = r.eh + 24 * j;
                for (int t = 0; t < 3; ++t) a.slots.push_back({h + 8 * t - s, true});
                const std::int64_t d = rd<std::int64_t>(b, h + 16);
                a.slots.push_back({d + 8 - s, true});
            }
            for (std::int32_t g = 0; g < r.gc; ++g) {
                const std::int64_t gp = r.eg + 32 * g;
                const std::int64_t n = rd<std::int64_t>(b, gp), vo = rd<std::int64_t>(b, gp + 8);
                a.slots.push_back({gp + 8 - s, true});
                a.slots.push_back({gp + 16 - s, true});
                for (std::int64_t t = 0; t < n; ++t) a.slots.push_back({vo + 4 * t - s, false});
            }
            const std::int64_t len = e - s;
            for (const Slot& sl : a.slots)
                if (sl.pos < 0 || sl.pos + (sl.wide ? 8 : 4) > len) throw TaeError("a pointer lies outside its payload");
            a.rel[0] = r.eh - s;
            a.rel[1] = r.eg - s;
            a.rel[2] = r.et - s;
            a.rel[3] = r.af - s;
            order.push_back(a.key);
            anims[a.key] = std::move(a);
        }
    }

    // The header with what a merge changes cleared: the file size and the
    // per-animation records, so two files can be told apart by the rest.
    std::vector<std::uint8_t> canon_head() const {
        std::vector<std::uint8_t> h = head;
        wr<std::int32_t>(h, 0xC, 0);
        for (std::int32_t i = 0; i < cnt; ++i) {
            const std::int64_t off = rd<std::int64_t>(h, ao + 16 * i + 8);
            if (off < 0 || static_cast<std::uint64_t>(off) + 0x30 > h.size()) throw TaeError("an animation record lies outside the header");
            std::memset(h.data() + off, 0, 0x30);
        }
        return h;
    }
};

Canon anim_canon(const Anim& a) {
    Canon c{a.ec, a.gc, a.tc, {a.rel[0], a.rel[1], a.rel[2], a.rel[3]}, a.payload};
    const std::int64_t s = a.start, len = static_cast<std::int64_t>(a.payload.size());
    for (const Slot& sl : a.slots) {
        if (sl.wide) {
            const std::int64_t v = rd<std::int64_t>(c.bytes, sl.pos);
            if (s <= v && v <= s + len) wr<std::int64_t>(c.bytes, sl.pos, v - s);
        } else {
            const std::int32_t v = rd<std::int32_t>(c.bytes, sl.pos);
            if (s <= v && v <= s + len) wr<std::int32_t>(c.bytes, sl.pos, static_cast<std::int32_t>(v - s));
        }
    }
    return c;
}

// The base's layout with the chosen animations' payloads put in place of its own.
std::vector<std::uint8_t> build(const Tae& base, const std::map<AnimKey, const Anim*>& chosen) {
    std::vector<std::uint8_t> head = base.head;
    std::vector<std::uint8_t> body;
    std::int64_t pos = static_cast<std::int64_t>(head.size());
    for (const AnimKey& key : base.order) {
        const auto pick = chosen.find(key);
        const Anim& a = pick != chosen.end() ? *pick->second : base.anims.at(key);
        const std::int64_t s = a.start, len = static_cast<std::int64_t>(a.payload.size());
        const std::int64_t delta = pos - s;
        if (delta % 16 != 0) throw TaeError("an animation's alignment changed");
        std::vector<std::uint8_t> p = a.payload;
        for (const Slot& sl : a.slots) {
            if (sl.wide) {
                const std::int64_t v = rd<std::int64_t>(p, sl.pos);
                if (s <= v && v <= s + len) wr<std::int64_t>(p, sl.pos, v + delta);
            } else {
                const std::int32_t v = rd<std::int32_t>(p, sl.pos);
                if (s <= v && v <= s + len) wr<std::int32_t>(p, sl.pos, static_cast<std::int32_t>(v + delta));
            }
        }
        body.insert(body.end(), p.begin(), p.end());
        const std::int64_t at = base.anims.at(key).struct_off;
        wr<std::int64_t>(head, at, pos + a.rel[0]);
        wr<std::int64_t>(head, at + 8, pos + a.rel[1]);
        wr<std::int64_t>(head, at + 16, pos + a.rel[2]);
        wr<std::int64_t>(head, at + 24, pos + a.rel[3]);
        wr<std::int32_t>(head, at + 32, a.ec);
        wr<std::int32_t>(head, at + 36, a.gc);
        wr<std::int32_t>(head, at + 40, a.tc);
        wr<std::int32_t>(head, at + 44, 0);
        pos += len;
    }
    std::vector<std::uint8_t> out = head;
    out.insert(out.end(), body.begin(), body.end());
    wr<std::int32_t>(out, 0xC, static_cast<std::int32_t>(out.size()));
    return out;
}

// ---- merging --------------------------------------------------------------

struct Ctx {
    AnibndReport& report;
    const AnibndLog& log;
    void say(const std::string& s) const {
        if (log) log(s);
    }
};

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

std::string anim_name(const AnimKey& k) {
    return std::to_string(k.first) + (k.second == 0 ? "" : "(#" + std::to_string(k.second) + ")");
}

struct ModTae {
    std::string label;
    std::unique_ptr<Tae> tae;
};

std::vector<std::uint8_t> tae_merge(const std::vector<std::uint8_t>& vdata,
                                    const std::vector<std::pair<std::string, const std::vector<std::uint8_t>*>>& mods,
                                    const Ctx& cx, const std::string& ename) {
    const Tae V(vdata);
    std::vector<ModTae> parsed;
    for (const auto& m : mods) parsed.push_back({m.first, std::make_unique<Tae>(*m.second)});
    const std::vector<std::uint8_t> vhead = V.canon_head();
    std::set<AnimKey> vkeys;
    for (const auto& kv : V.anims) vkeys.insert(kv.first);

    // The base layout. A mod that lacks some of the original's animations has
    // not deleted them, it just left them alone (older mods are built from an
    // older original), and the base must hold every animation a mod added.
    std::set<AnimKey> uni = vkeys;
    for (const auto& p : parsed)
        for (const auto& kv : p.tae->anims) uni.insert(kv.first);
    struct Cand {
        const std::string* label;  // nullptr: the original
        const Tae* tae;
    };
    std::vector<Cand> cands{{nullptr, &V}};
    for (const auto& p : parsed) cands.push_back({&p.label, p.tae.get()});
    auto keys_of = [](const Tae& t) {
        std::set<AnimKey> s;
        for (const auto& kv : t.anims) s.insert(kv.first);
        return s;
    };
    std::vector<Cand> full;
    for (const Cand& c : cands)
        if (keys_of(*c.tae) == uni) full.push_back(c);
    Cand base{nullptr, &V};
    if (!full.empty()) {
        std::vector<Cand> hd;
        for (const Cand& c : full)
            if (c.label && c.tae->canon_head() != vhead) hd.push_back(c);
        if (!hd.empty()) {
            base = hd.back();
        } else {
            base = full[0];
            for (const Cand& c : full)
                if (!c.label) {
                    base = c;
                    break;
                }
        }
        if (hd.size() > 1) cx.say("  warning: " + ename + ": more than one mod has a different header; using the header of " + *base.label + ", the highest priority");
    } else {
        // Mods added different new animations: take the file holding the most.
        std::size_t best = 0;
        std::pair<std::size_t, bool> best_key{0, false};
        for (std::size_t i = 0; i < cands.size(); ++i) {
            std::size_t n = 0;
            for (const auto& k : keys_of(*cands[i].tae))
                if (uni.count(k)) ++n;
            const std::pair<std::size_t, bool> key{n, cands[i].label == nullptr};
            if (i == 0 || key > best_key) {
                best = i;
                best_key = key;
            }
        }
        base = cands[best];
        std::size_t miss = 0;
        for (const AnimKey& k : uni)
            if (!base.tae->anims.count(k)) ++miss;
        cx.say("  warning: " + ename + ": the mods add different new animations so one layout cannot hold them all; based on " +
               (base.label ? *base.label : std::string("the original")) + ", " + std::to_string(miss) + " new animations left out");
        cx.report.warnings.push_back(ename + ": mods add different new animations; " + std::to_string(miss) + " left out of the result");
    }
    {
        std::vector<std::string> lacking;
        std::vector<std::string> counts;
        for (const auto& p : parsed) {
            std::size_t missing = 0;
            for (const AnimKey& k : vkeys)
                if (!p.tae->anims.count(k)) ++missing;
            if (missing) {
                lacking.push_back(p.label);
                counts.push_back(std::to_string(missing));
            }
        }
        if (!lacking.empty())
            cx.say("  " + ename + ": " + join(lacking, ", ") + " lacks " + join(counts, "/") +
                   " of the original's animations; those count as unchanged (the original's or another mod's are kept)");
    }
    if (base.label) cx.say("  " + ename + ": layout taken from " + *base.label + " (" + std::to_string(base.tae->anims.size()) + " animations)");

    struct Claim {
        std::size_t mod;
        const Anim* anim;
        Canon canon;
    };
    std::map<AnimKey, Claim> claims;
    std::map<AnimKey, std::vector<std::string>> claimers;
    std::map<AnimKey, Canon> vcanon;
    for (const auto& kv : V.anims) vcanon.emplace(kv.first, anim_canon(kv.second));
    int nconf = 0;
    std::vector<std::pair<std::string, int>> changed;
    for (std::size_t mi = 0; mi < parsed.size(); ++mi) {
        const ModTae& m = parsed[mi];
        int n = 0;
        for (const AnimKey& key : m.tae->order) {
            const Anim& a = m.tae->anims.at(key);
            Canon c = anim_canon(a);
            const auto vc = vcanon.find(key);
            if (vc != vcanon.end() && vc->second == c) continue;
            ++n;
            const auto prev = claims.find(key);
            if (prev != claims.end() && prev->second.canon != c) {
                ++nconf;
                cx.say("  CONFLICT " + ename + " animation " + anim_name(key) + ": " + m.label + " overrides " + parsed[prev->second.mod].label);
                AnibndConflict cf;
                cf.entry = ename;
                cf.anim = key.first;
                cf.index = key.second;
                cf.winner = m.label;
                for (const std::string& l : claimers[key])
                    if (l != m.label) cf.losers.push_back(l);
                cx.report.conflicts.push_back(std::move(cf));
            }
            claims.erase(key);
            claims.emplace(key, Claim{mi, &a, std::move(c)});
            claimers[key].push_back(m.label);
        }
        changed.emplace_back(m.label, n);
    }
    std::map<AnimKey, const Anim*> chosen;
    int dropped = 0;
    for (const auto& kv : claims) {
        const auto b = base.tae->anims.find(kv.first);
        if (b == base.tae->anims.end()) {
            ++dropped;
            continue;
        }
        if (kv.second.anim != &b->second) chosen[kv.first] = kv.second.anim;
    }
    if (dropped) {
        cx.say("  warning: " + ename + ": " + std::to_string(dropped) + " animations the layout does not have were left out");
        cx.report.warnings.push_back(ename + ": " + std::to_string(dropped) + " animations the layout does not have were left out");
    }
    std::vector<std::uint8_t> out = build(*base.tae, chosen);
    // Read the result back and check every animation is what was chosen.
    const Tae chk(out);
    if (chk.order != base.tae->order || rd<std::int32_t>(out, 0xC) != static_cast<std::int32_t>(out.size()))
        throw TaeError("self-check failed (layout)");
    for (const AnimKey& key : base.tae->order) {
        const auto pick = chosen.find(key);
        const Canon want = anim_canon(pick != chosen.end() ? *pick->second : base.tae->anims.at(key));
        if (anim_canon(chk.anims.at(key)) != want) throw TaeError("self-check failed (animation " + anim_name(key) + ")");
    }
    std::string summary;
    for (const auto& c : changed) summary += (summary.empty() ? "" : ", ") + c.first + " " + std::to_string(c.second);
    cx.say("  " + ename + ": merged animation by animation (" + summary + ") / conflicts " + std::to_string(nconf));
    return out;
}

// ---- names ----------------------------------------------------------------

std::string utf8_from_utf16le(const std::vector<std::uint8_t>& n) {
    std::string s;
    for (std::size_t i = 0; i + 1 < n.size(); i += 2) {
        std::uint32_t c = n[i] | (static_cast<std::uint32_t>(n[i + 1]) << 8);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n.size()) {
            const std::uint32_t d = n[i + 2] | (static_cast<std::uint32_t>(n[i + 3]) << 8);
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

// The name a report shows: the last path part, with the folder before it for
// an .hkx (the same file name, c0000.hkx, sits in several folders).
std::string short_name(const std::string& n) {
    std::vector<std::string> parts;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = n.find('\\', from);
        if (at == std::string::npos) {
            parts.push_back(n.substr(from));
            break;
        }
        parts.push_back(n.substr(from, at - from));
        from = at + 1;
    }
    std::string last = parts.back();
    std::string lower = last;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".hkx") == 0 && parts.size() > 1) return parts[parts.size() - 2] + "/" + last;
    return last;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads a DCX (or a bare BND4) into an archive the merge can use.
bool read_archive(const std::vector<std::uint8_t>& file, bool must_be_dcx, gcn::Bnd4Archive& out, std::string* error) {
    if (must_be_dcx && (file.size() < 4 || std::memcmp(file.data(), "DCX\0", 4) != 0)) {
        if (error) *error = "the original must be a DCX file (c0000.anibnd.dcx)";
        return false;
    }
    std::string why;
    const std::vector<std::uint8_t> raw = gcn::dcx_decompress(file, &why);
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

}  // namespace

bool anibnd_merge(const std::vector<std::uint8_t>& original, const std::vector<AnibndMod>& mods, bool dry,
                  AnibndReport& report, std::vector<std::uint8_t>* out, std::string* error, const AnibndLog& log, int compress_level) {
    report = AnibndReport{};
    if (out) out->clear();
    const Ctx cx{report, log};
    try {
        gcn::Bnd4Archive V;
        std::string why;
        if (!read_archive(original, true, V, &why)) {
            if (error) *error = "original: " + why;
            return false;
        }
        struct ModFiles {
            std::string label;
            std::vector<std::string> names;  // in order of first appearance
            std::map<std::string, const gcn::Bnd4File*> by_name;
        };
        std::vector<gcn::Bnd4Archive> archives(mods.size());
        std::vector<ModFiles> M;
        std::set<std::string> used;
        for (std::size_t i = 0; i < mods.size(); ++i) {
            if (!read_archive(mods[i].file, false, archives[i], &why)) {
                if (error) *error = mods[i].label + ": " + why;
                return false;
            }
        }
        for (std::size_t i = 0; i < mods.size(); ++i) {
            ModFiles m;
            m.label = mods[i].label;
            while (used.count(m.label)) m.label += '+';
            used.insert(m.label);
            for (const gcn::Bnd4File& f : archives[i].files) {
                const std::string n = utf8_from_utf16le(f.name);
                if (!m.by_name.count(n)) m.names.push_back(n);
                m.by_name[n] = &f;  // the last one of a name wins
            }
            M.push_back(std::move(m));
        }
        std::map<std::string, const gcn::Bnd4File*> vmap;
        std::vector<std::string> names;
        for (const gcn::Bnd4File& f : V.files) {
            const std::string n = utf8_from_utf16le(f.name);
            names.push_back(n);
            vmap[n] = &f;
        }
        for (const ModFiles& m : M) {
            int nd = 0;
            for (const auto& kv : m.by_name) {
                const auto v = vmap.find(kv.first);
                if (v == vmap.end() || kv.second->data != v->second->data) ++nd;
            }
            report.diffs.emplace_back(m.label, nd);
            if (nd == 0) {
                const std::string w = m.label + " does not differ from the original: the original is probably already modified by it, or is not the unmodified one (overlaps cannot be seen then)";
                cx.say("warning: " + w);
                report.warnings.push_back(w);
            }
        }
        {
            std::set<std::string> have(names.begin(), names.end());
            for (const ModFiles& m : M)
                for (const std::string& n : m.names)
                    if (!vmap.count(n) && !have.count(n)) {
                        names.push_back(n);
                        have.insert(n);
                    }
        }
        gcn::Bnd4Archive res;
        res.head = V.head;
        res.entry_size = V.entry_size;
        res.unicode = V.unicode;
        for (const std::string& n : names) {
            const auto ve = vmap.find(n);
            const gcn::Bnd4File* orig = ve == vmap.end() ? nullptr : ve->second;
            std::vector<std::pair<std::string, const gcn::Bnd4File*>> ch;
            for (const ModFiles& m : M) {
                const auto f = m.by_name.find(n);
                if (f != m.by_name.end() && (!orig || f->second->data != orig->data)) ch.emplace_back(m.label, f->second);
            }
            const std::string sn = short_name(n);
            if (ch.size() > 1) {
                bool same = true;
                for (const auto& c : ch) same = same && c.second->data == ch[0].second->data;
                if (same) ch.erase(ch.begin(), ch.end() - 1);  // the same change from several mods is not a conflict
            }
            if (ch.empty()) {
                res.files.push_back(*orig);
                continue;
            }
            if (ch.size() == 1) {
                res.files.push_back(*ch[0].second);
                ++report.entries_taken;
                cx.say(sn + " <- " + ch[0].first);
                report.taken.push_back({sn, {ch[0].first}});
                continue;
            }
            if (orig && ends_with(n, ".tae")) {
                try {
                    std::vector<std::pair<std::string, const std::vector<std::uint8_t>*>> in;
                    for (const auto& c : ch) in.emplace_back(c.first, &c.second->data);
                    gcn::Bnd4File merged = *ch.back().second;
                    merged.data = tae_merge(orig->data, in, cx, sn);
                    res.files.push_back(std::move(merged));
                    ++report.entries_merged;
                    continue;
                } catch (const std::exception& ex) {
                    cx.say("  warning: " + sn + ": animation-by-animation merge failed (" + ex.what() + "); the whole entry goes to the later mod");
                }
            }
            ++report.entries_conflicting;
            AnibndConflict cf;
            cf.entry = sn;
            cf.winner = ch.back().first;
            for (std::size_t i = 0; i + 1 < ch.size(); ++i) cf.losers.push_back(ch[i].first);
            cx.say("CONFLICT " + sn + ": " + cf.winner + " overwrites the whole entry of " + join(cf.losers, ", ") + " (not mergeable)");
            report.conflicts.push_back(std::move(cf));
            res.files.push_back(*ch.back().second);
        }
        cx.say("done: taken as is " + std::to_string(report.entries_taken) + ", merged animation by animation " + std::to_string(report.entries_merged) +
               ", whole-entry conflicts " + std::to_string(report.entries_conflicting));
        if (!dry && out) {
            const std::uint32_t n = static_cast<std::uint32_t>(res.files.size());
            std::memcpy(res.head.data() + 0xc, &n, 4);  // bnd4_pack keeps the header as given
            const std::vector<std::uint8_t> packed = gcn::dcx_compress(gcn::bnd4_pack(res), compress_level);
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
