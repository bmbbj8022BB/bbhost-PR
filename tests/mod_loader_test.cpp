// mods/mod_loader on a game folder, mods and an overlay built in a temporary
// folder: matching by path, zip mods (also a hostile one), apply, apply again,
// a changed mod, turning a mod off, revert (a file the overlay already had comes
// back), the record (state.json) and the folder checks. No game files.
#include "gcn/container.h"
#include "mods/mod_loader.h"
#include "mods/zip_reader.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mods;

namespace {

int g_failed = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                  \
        }                                                                \
    } while (0)

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A zip with stored entries (and one deflated), written by hand.
void put_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files, bool deflate_first = false) {
    std::string out, cd;
    int n = 0;
    auto w16 = [](std::string& s, unsigned v) { s += static_cast<char>(v & 255); s += static_cast<char>((v >> 8) & 255); };
    auto w32 = [&](std::string& s, unsigned v) { w16(s, v & 0xFFFF); w16(s, v >> 16); };
    for (const auto& [name, data] : files) {
        std::string body = data;
        unsigned method = 0;
        if (deflate_first && n == 0) {
            z_stream z{};
            deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            std::string c(data.size() + 64, '\0');
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
            z.avail_in = static_cast<uInt>(data.size());
            z.next_out = reinterpret_cast<Bytef*>(&c[0]);
            z.avail_out = static_cast<uInt>(c.size());
            deflate(&z, Z_FINISH);
            c.resize(z.total_out);
            deflateEnd(&z);
            body = c;
            method = 8;
        }
        const unsigned crc = static_cast<unsigned>(crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
        const unsigned off = static_cast<unsigned>(out.size());
        w32(out, 0x04034b50); w16(out, 20); w16(out, 0x800); w16(out, method); w16(out, 0); w16(out, 0);
        w32(out, crc); w32(out, static_cast<unsigned>(body.size())); w32(out, static_cast<unsigned>(data.size()));
        w16(out, static_cast<unsigned>(name.size())); w16(out, 0);
        out += name; out += body;
        w32(cd, 0x02014b50); w16(cd, 20); w16(cd, 20); w16(cd, 0x800); w16(cd, method); w16(cd, 0); w16(cd, 0);
        w32(cd, crc); w32(cd, static_cast<unsigned>(body.size())); w32(cd, static_cast<unsigned>(data.size()));
        w16(cd, static_cast<unsigned>(name.size())); w16(cd, 0); w16(cd, 0); w16(cd, 0); w16(cd, 0); w32(cd, 0); w32(cd, off);
        cd += name;
        ++n;
    }
    const unsigned cd_off = static_cast<unsigned>(out.size());
    out += cd;
    w32(out, 0x06054b50); w16(out, 0); w16(out, 0); w16(out, static_cast<unsigned>(n)); w16(out, static_cast<unsigned>(n));
    w32(out, static_cast<unsigned>(cd.size())); w32(out, cd_off); w16(out, 0);
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << out;
}


// ---- archives like the game's anibnd (BND4, UTF-16 names, in a DCX)
Bytes utf16(const std::string& s) {
    Bytes n;
    for (char c : s) { n.push_back(static_cast<std::uint8_t>(c)); n.push_back(0); }
    return n;
}
Bytes make_anibnd(const std::vector<std::pair<std::string, Bytes>>& entries) {
    gcn::Bnd4Archive a;
    a.head.assign(0x40, 0);
    std::memcpy(a.head.data(), "BND4", 4);
    a.head[0x30] = 1;
    a.head[0x31] = 0x74;
    const std::int64_t v10 = 0x40, v20 = 0x24;
    std::memcpy(a.head.data() + 0x10, &v10, 8);
    std::memcpy(a.head.data() + 0x20, &v20, 8);
    const std::int32_t cnt = static_cast<std::int32_t>(entries.size());
    std::memcpy(a.head.data() + 0xC, &cnt, 4);
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
    return gcn::dcx_compress(gcn::bnd4_pack(a));
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
void put_bytes(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
Bytes bytes_of(const std::string& s) { return Bytes(s.begin(), s.end()); }

std::vector<ModSource> enabled(std::vector<ModSource> v) {
    std::vector<ModSource> o;
    for (auto& m : v)
        if (m.enabled) o.push_back(m);
    return o;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "mod_loader_test";
    fs::remove_all(root);
    const fs::path game = root / "game", upd = root / "game-patch", mods_dir = root / "mods", overlay = root / "overlay";
    const std::string sd = (mods_dir / ".bbmods").string();

    // The game: a base folder and its update (which has the same file, newer).
    put(game / "dvdroot_ps4/chr/c0000.txtbin", "base-c0000");
    put(game / "dvdroot_ps4/menu/menu.dat", "base-menu");
    put(game / "dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx", "base-gameparam");
    put(upd / "dvdroot_ps4/menu/menu.dat", "update-menu");
    put(game / "dvdroot_ps4/msg/engus/item.msgbnd.dcx", "base-msg");
    put(game / "dvdroot_ps4/msg/engus/menu.msgbnd.dcx", "base-msg2");
    CHECK(find_update_folder(game.string()) == upd.string());

    // The overlay already has a file of the player's own.
    put(overlay / "dvdroot_ps4/menu/menu.dat", "player-own");

    // Mod A: a folder without the game's folders. Mod B: a zip with a different layout. Mod C: a hostile zip.
    put(mods_dir / "A mod/menu.dat", "A-menu");
    put(mods_dir / "A mod/readme.txt", "ignore me");
    put(mods_dir / "A mod/newdir/notthere.bin", "A-unmatched");
    put_zip(mods_dir / "B mod.zip", {{"chr/c0000.txtbin", "B-c0000"}, {"msg/engus/item.msgbnd.dcx", "B-msg"}}, true);
    put_zip(mods_dir / "C evil.zip", {{"../../escape.txt", "x"}, {"dvdroot_ps4/menu/menu.dat", "C-menu"}, {"C:/windows/x.bin", "y"}});

    // ---- zip reader
    {
        ZipReader z;
        std::string why;
        CHECK(z.open((mods_dir / "B mod.zip").string(), &why));
        CHECK(z.entries().size() == 2);
        Bytes b;
        CHECK(z.read(z.entries()[0], &b, &why));
        CHECK(std::string(b.begin(), b.end()) == "B-c0000");
        ZipReader bad;
        put(root / "notzip.zip", "this is not a zip");
        CHECK(!bad.open((root / "notzip.zip").string(), &why));
    }

    // ---- mod list: new mods come last, off; the saved order and switches come back
    std::vector<ModSource> list = list_mods(mods_dir.string());
    CHECK(list.size() == 3);
    CHECK(list[0].name == "A mod" && !list[0].is_zip && list[1].is_zip);
    for (auto& m : list) m.enabled = (m.name != "C evil.zip");
    std::swap(list[0], list[1]);   // B first, A second: A wins menu.dat
    std::string why;
    CHECK(save_modlist(mods_dir.string(), list, &why));
    list = list_mods(mods_dir.string());
    CHECK(list[0].name == "B mod.zip" && list[1].name == "A mod" && list[2].name == "C evil.zip");
    CHECK(list[0].enabled && list[1].enabled && !list[2].enabled);

    // ---- the hostile zip never leaves the overlay
    {
        const auto files = list[2].files();
        CHECK(files.size() == 1 && files[0].rel() == "dvdroot_ps4/menu/menu.dat");
    }

    // ---- matching and the plan
    GameIndex idx;
    idx.build({game.string(), upd.string()}, {overlay.string(), mods_dir.string()});
    CHECK(idx.files.size() == 5);   // the update's menu.dat replaces the game's
    {
        const auto res = resolve_mod(idx, "A mod", list[1].files());
        bool menu = false, doc = false, unmatched = false;
        for (const Resolved& r : res) {
            if (r.ref.parts.back() == "menu.dat") menu = r.status == Status::Exact && r.rel == "dvdroot_ps4/menu/menu.dat";
            if (r.ref.parts.back() == "readme.txt") doc = r.status == Status::Doc;
            if (r.ref.parts.back() == "notthere.bin") unmatched = r.status == Status::Unmatched || r.status == Status::Mapped;
        }
        CHECK(menu && doc && unmatched);
    }
    MergeEnv env;   // no paramdef: params are not merged
    Plan plan = make_plan(enabled(list), idx, overlay.string());
    // A's menu.dat, A's new file (put beside menu.dat by the mod's own rule), B's c0000 and B's item.msgbnd.dcx.
    CHECK(plan.targets.size() == 4);
    CHECK(plan.ambiguous.empty());

    // ---- apply
    std::vector<std::string> log;
    auto logger = [&](const std::string& l) { log.push_back(l); };
    ApplyStats s1 = apply_plan(plan, sd, env, logger);
    CHECK(s1.copied == 4 && s1.error == 0 && s1.same == 0);
    CHECK(get(overlay / "dvdroot_ps4/menu/menu.dat") == "A-menu");        // A is after B... and the only one on menu.dat
    CHECK(get(overlay / "dvdroot_ps4/chr/c0000.txtbin") == "B-c0000");
    CHECK(get(overlay / "dvdroot_ps4/msg/engus/item.msgbnd.dcx") == "B-msg");
    CHECK(!fs::exists(overlay / "readme.txt"));
    CHECK(!fs::exists(root / "escape.txt") && !fs::exists(root.parent_path() / "escape.txt"));
    // The game folder is untouched.
    CHECK(get(game / "dvdroot_ps4/menu/menu.dat") == "base-menu");
    CHECK(get(upd / "dvdroot_ps4/menu/menu.dat") == "update-menu");

    // The record is the Python loader's state.json, and says what was applied.
    {
        const std::string js = get(fs::path(sd) / "state.json");
        CHECK(js.find("\"entries\"") != std::string::npos && js.find("menu.dat") != std::string::npos);
        const auto [names, order] = applied_info(sd);
        CHECK(names.size() == 2 && order.size() == 2 && order[0] == "B mod.zip");
        // the player's own menu.dat was saved first
        bool saved = false;
        for (auto& e : fs::recursive_directory_iterator(fs::path(sd) / "backup")) saved = saved || (e.is_regular_file() && get(e.path()) == "player-own");
        CHECK(saved);
    }

    // ---- again: nothing is touched
    ApplyStats s2 = apply_plan(make_plan(enabled(list), idx, overlay.string()), sd, env, logger);
    CHECK(s2.same == 4 && s2.copied == 0 && s2.merged == 0 && s2.error == 0);

    // ---- a mod's file changes: only that file is written again
    put(mods_dir / "A mod/menu.dat", "A-menu-v2");
    ApplyStats s3 = apply_plan(make_plan(enabled(list), idx, overlay.string()), sd, env, logger);
    CHECK(s3.copied == 1 && s3.same == 3);
    CHECK(get(overlay / "dvdroot_ps4/menu/menu.dat") == "A-menu-v2");

    // ---- two mods on one non-mergeable file: the later wins, and it is a conflict
    list[2].enabled = true;   // C evil.zip, last: its menu.dat wins over A's
    {
        const Plan p = make_plan(enabled(list), idx, overlay.string());
        const Preview pv = preview_merges(p, env);
        CHECK(pv.overwritten.size() == 1 && pv.overwritten[0].second.back() == "C evil.zip");
        CHECK(pv.has_conflicts());
        ApplyStats s4 = apply_plan(p, sd, env, logger);
        CHECK(s4.conflict == 1);
        CHECK(get(overlay / "dvdroot_ps4/menu/menu.dat") == "C-menu");
    }

    // ---- a mod turned off: what only it wrote goes away, a shared file goes back to the next mod's
    list[2].enabled = false;
    list[1].enabled = false;   // A off: its menu.dat and its new file are no longer wanted
    {
        ApplyStats s5 = apply_plan(make_plan(enabled(list), idx, overlay.string()), sd, env, logger);
        CHECK(s5.reverted == 2);
        // menu.dat is no longer written by any mod: the player's own file is back
        CHECK(get(overlay / "dvdroot_ps4/menu/menu.dat") == "player-own");
        CHECK(get(overlay / "dvdroot_ps4/chr/c0000.txtbin") == "B-c0000");
    }

    // ---- revert all: exactly what was written goes, the player's own file stays
    {
        const int n = revert_all(sd, logger);
        CHECK(n == 2);
        CHECK(!fs::exists(overlay / "dvdroot_ps4/chr"));          // the folder it made is gone too
        CHECK(!fs::exists(overlay / "dvdroot_ps4/msg"));
        CHECK(get(overlay / "dvdroot_ps4/menu/menu.dat") == "player-own");
        CHECK(applied_info(sd).first.empty());
        // and the game is still as it was
        CHECK(get(game / "dvdroot_ps4/chr/c0000.txtbin") == "base-c0000");
    }

    // ---- a record that points outside its folder is refused, not followed
    {
        put(fs::path(sd) / "state.json", "{\"entries\": {\"k\": {\"root\": \"" + overlay.generic_string() + "\", \"rel\": \"../victim.txt\", \"sig\": \"\", \"backup\": false, \"mods\": []}}, \"order\": []}");
        put(root / "victim.txt", "keep me");
        revert_all(sd, logger);
        CHECK(get(root / "victim.txt") == "keep me");
    }

    // ---- folders that cannot be used together
    CHECK(check_folders(game.string(), overlay.string(), mods_dir.string()) == FolderProblem::None);
    CHECK(check_folders(game.string(), (mods_dir / "inner").string(), mods_dir.string()) == FolderProblem::OverlayInsideOrContainsMods);
    CHECK(check_folders(game.string(), (game / "dvdroot_ps4").string(), mods_dir.string()) == FolderProblem::OverlayInsideOrContainsGame);
    CHECK(check_folders(game.string(), upd.string(), mods_dir.string()) == FolderProblem::OverlayInsideOrContainsGame);
    CHECK(check_folders(game.string(), "", mods_dir.string()) == FolderProblem::OverlayEmpty);

    // ---- importing: a zip and a folder are copied in, last and on; a name taken gets a number
    {
        put_zip(root / "incoming/New one.zip", {{"menu/menu.dat", "N"}});
        put(root / "incoming/Folder mod/menu.dat", "F");
        const auto added = import_mods({(root / "incoming/New one.zip").string(), (root / "incoming/Folder mod").string(), (root / "incoming/New one.zip").string()}, mods_dir.string(), logger);
        CHECK(added.size() == 3);
        CHECK(added[2] == "New one (2).zip");
        const auto l2 = list_mods(mods_dir.string());
        CHECK(l2.size() == 6 && l2[3].enabled && l2[4].enabled && l2[5].enabled);
        CHECK(l2[0].name == "B mod.zip");   // the old order is kept
    }

    // ---- merging: two mods change different entries of one anibnd, a third one the same entry as the first
    {
        const fs::path g2 = root / "g2", o2 = root / "o2", m2 = root / "m2";
        const std::string kBeh = "chr\\c0000\\hkx\\Behaviors\\c0000.hkx", kSkel = "chr\\c0000\\hkx\\Skeletons\\c0000.hkx";
        const Bytes vanilla = make_anibnd({{kBeh, bytes_of("beh-vanilla")}, {kSkel, bytes_of("skel-vanilla")}});
        put_bytes(g2 / "dvdroot_ps4/chr/c0000.anibnd.dcx", vanilla);
        put_bytes(m2 / "ModA/chr/c0000.anibnd.dcx", make_anibnd({{kBeh, bytes_of("beh-A")}, {kSkel, bytes_of("skel-vanilla")}}));
        put_bytes(m2 / "ModB/chr/c0000.anibnd.dcx", make_anibnd({{kBeh, bytes_of("beh-vanilla")}, {kSkel, bytes_of("skel-B")}}));
        put_bytes(m2 / "ModC/chr/c0000.anibnd.dcx", make_anibnd({{kBeh, bytes_of("beh-C")}, {kSkel, bytes_of("skel-vanilla")}}));
        GameIndex i2;
        i2.build({g2.string()}, {o2.string(), m2.string()});
        std::vector<ModSource> ms = list_mods(m2.string());
        CHECK(ms.size() == 3);
        for (auto& m : ms) m.enabled = m.name != "ModC";
        const std::string sd2 = (m2 / ".bbmods").string();
        {
            const Plan p = make_plan(enabled(ms), i2, o2.string());
            CHECK(p.targets.size() == 1 && fs::path(p.targets[0].second.orig) == g2 / "dvdroot_ps4/chr/c0000.anibnd.dcx");
            const Preview pv = preview_merges(p, env);
            CHECK(pv.items.size() == 1 && pv.items[0].conflicts.empty() && !pv.has_conflicts());
            const ApplyStats st = apply_plan(p, sd2, env, logger);
            CHECK(st.merged == 1 && st.copied == 0 && st.error == 0);
            const auto e = entries_of(Bytes(bytes_of(get(o2 / "dvdroot_ps4/chr/c0000.anibnd.dcx"))));
            CHECK(e.size() == 2 && e.at(kBeh) == bytes_of("beh-A") && e.at(kSkel) == bytes_of("skel-B"));
            CHECK(apply_plan(p, sd2, env, logger).same == 1);   // the same merge is not redone
        }
        // ModC changes what ModA changed: C is last, so C wins that entry, B's change stays, and it is said so.
        for (auto& m : ms) m.enabled = true;
        {
            const Plan p = make_plan(enabled(ms), i2, o2.string());
            const Preview pv = preview_merges(p, env);
            CHECK(pv.items.size() == 1 && pv.items[0].conflicts.size() == 1 && pv.has_conflicts());
            CHECK(pv.items[0].conflicts[0].find("ModC wins") != std::string::npos);
            const ApplyStats st = apply_plan(p, sd2, env, logger);
            CHECK(st.merged == 1);
            const auto e = entries_of(Bytes(bytes_of(get(o2 / "dvdroot_ps4/chr/c0000.anibnd.dcx"))));
            CHECK(e.at(kBeh) == bytes_of("beh-C") && e.at(kSkel) == bytes_of("skel-B"));
            CHECK(get(fs::path(sd2) / "merge.log").find("Merge: dvdroot_ps4/chr/c0000.anibnd.dcx") != std::string::npos);
        }
        CHECK(revert_all(sd2, logger) == 1);
        CHECK(!fs::exists(o2 / "dvdroot_ps4"));
        // the game's own file was never touched
        CHECK(get(g2 / "dvdroot_ps4/chr/c0000.anibnd.dcx") == std::string(vanilla.begin(), vanilla.end()));
    }

    fs::remove_all(root);
    std::printf(g_failed ? "mod_loader_test: %d failed\n" : "mod_loader_test: all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
