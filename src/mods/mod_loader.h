// The mod loader's engine: everything the setup window's Mods page does, with
// no window in it (a port of the Python BBModLoader's modengine.py).
//
//  - a mods folder: each sub-folder or .zip in it is one mod; which are on and
//    in what order is kept in <mods folder>/.bbmods/modlist.txt;
//  - a mod's files are matched to the game's by path, whether or not the mod
//    keeps the game's folders (chr/c0000.anibnd.dcx finds
//    dvdroot_ps4/chr/c0000.anibnd.dcx);
//  - the enabled mods, in list order (a later one wins), are written into the
//    overlay folder (bbhost's paths.mods) - never into the game folder;
//    *.anibnd.dcx and gameparam.parambnd.dcx that several mods change are
//    merged (mods/anibnd_merge.h, mods/param_merge.h), anything else the
//    later mod's file wins whole;
//  - every file written is recorded in <mods folder>/.bbmods/state.json (the
//    Python loader's format), a file that was already in the overlay is saved
//    first, and "revert all" removes exactly what the record lists and puts
//    the saved ones back. Nothing outside the overlay folder is written.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mods {

class ZipReader;
struct ZipEntry;

using Bytes = std::vector<std::uint8_t>;
using LogFn = std::function<void(const std::string&)>;

// ------------------------------------------------------------------ mods
struct FileRef {
    std::vector<std::string> parts;      // the path inside the mod
    std::string src;                     // the file (folder mod) or the zip
    std::shared_ptr<ZipReader> zip;      // set for a zip's entry
    std::size_t zip_index = 0;
    std::uint64_t size = 0;
    std::string sig;                     // changes when the file does
    std::string rel() const;
    bool read(Bytes* out, std::string* error) const;
    bool write_to(const std::string& dst, std::string* error) const;  // via a temporary, then renamed
};

struct ModSource {
    std::string path, name;
    bool is_zip = false;
    bool enabled = false;
    // Every file in the mod, sorted by lower-case path. Entries that would leave
    // the overlay ('..', a drive letter) are left out.
    std::vector<FileRef> files(std::string* error = nullptr) const;
};

// The mods in `mods_dir` (sub-folders and .zip files, not starting with '.' or
// '_'), in the saved order; mods that are new come last, off.
std::vector<ModSource> list_mods(const std::string& mods_dir);
// Saves which mods are on and their order.
bool save_modlist(const std::string& mods_dir, const std::vector<ModSource>& mods, std::string* error);
// Copies zip files / mod folders into the mods folder (they come out on, last).
// The names added.
std::vector<std::string> import_mods(const std::vector<std::string>& paths, const std::string& mods_dir, const LogFn& log);

// ------------------------------------------------------------------ the game
struct GameIndex {
    struct File {
        std::string rel, lower;   // actual / lower-case path under its root
        int root = 0;
    };
    std::vector<std::string> roots;                                  // [game, (update)]: later wins
    std::vector<File> files;
    std::map<std::string, std::vector<std::size_t>> by_name;         // last component (lower) -> files, sorted by path
    std::map<std::string, std::vector<std::string>> dirs_by_name;    // last component (lower) -> folders (lower)
    std::map<std::string, std::string> dirs;                         // lower-case folder -> actual
    std::map<std::string, bool> dirnames;                            // every folder component, lower case
    std::map<std::string, std::size_t> by_lower;                     // lower-case path -> files
    // The files whose path ends with the components L[from:] / the folders whose path ends with L[from:to].
    std::vector<std::size_t> find_files(const std::vector<std::string>& L, std::size_t from) const;
    std::vector<std::string> find_dirs(const std::vector<std::string>& L, std::size_t from, std::size_t to) const;
    // `exclude`: folders that are not the game (the overlay, the mods folder).
    void build(const std::vector<std::string>& roots, const std::vector<std::string>& exclude, const LogFn& progress = nullptr);
    std::string full_path(const File& f) const;
};

// The game folder's update (CUSA00900-UPDATE, CUSA00900-patch), or "".
std::string find_update_folder(const std::string& app0);

// ------------------------------------------------------------------ matching
enum class Status { Exact, Picked, Mapped, Dir, Doc, Unmatched, Ambiguous, Skipped };

struct Resolved {
    FileRef ref;
    Status status = Status::Unmatched;
    std::string rel;                     // under the game folder ("" when none)
    std::vector<std::string> cands;      // Ambiguous: the candidates
    std::string key;                     // the choice's key
};

constexpr const char* kSkip = "__skip__";
// choices: key ("<mod>|<lower-case path>") -> the game path chosen, or kSkip.
std::vector<Resolved> resolve_mod(const GameIndex& index, const std::string& mod_name, const std::vector<FileRef>& files,
                                  const std::map<std::string, std::string>& choices = {});

// ------------------------------------------------------------------ plan
struct Target {
    std::string root, rel, orig;                       // orig: the game's own file at rel ("" when new)
    std::vector<std::pair<std::string, FileRef>> sources;   // (mod, file) in application order
};

struct Plan {
    std::string overlay_root;
    std::vector<std::string> order;                    // the mods, in application order
    std::vector<std::pair<std::string, Target>> targets;   // key -> target, in first-seen order
    std::map<std::string, std::size_t> target_at;          // key -> its place in targets
    std::vector<std::pair<std::string, Resolved>> ambiguous;
    std::vector<std::pair<std::string, std::vector<Resolved>>> per_mod;
    const Target* find(const std::string& key) const;
};

// What merging needs from the game besides the originals.
struct MergeEnv {
    Bytes paramdef;                    // dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx; empty: params are not merged
    int compress_level = 6;
};

// `mods`: the enabled mods in application order. Only reads the file system.
Plan make_plan(const std::vector<ModSource>& mods, const GameIndex& index, const std::string& overlay_root,
               const std::map<std::string, std::string>& choices = {});
// The game's paramdef from the index (empty when it has none).
Bytes load_paramdef(const GameIndex& index);

bool can_merge(const std::string& rel, bool param_ok);

// ------------------------------------------------------------------ preview
struct PreviewItem {
    std::string rel, base;
    std::vector<std::string> mods, conflicts, warnings, diffs;
    std::string error;
};
struct Preview {
    std::vector<PreviewItem> items;                                         // merged files
    std::vector<std::pair<std::string, std::vector<std::string>>> overwritten;   // file, mods: the last wins whole
    std::vector<std::pair<std::string, std::vector<std::string>>> mergeable;     // every merge target
    std::vector<std::string> errors;
    bool has_conflicts() const;
    std::vector<std::string> conflict_lines() const;
    std::vector<std::string> diagnostics() const;
};
Preview preview_merges(const Plan& plan, const MergeEnv& env, const LogFn& log = nullptr);

// ------------------------------------------------------------------ apply / revert
struct ApplyStats {
    int same = 0, copied = 0, merged = 0, conflict = 0, reverted = 0, error = 0;
};
// state_dir: <mods folder>/.bbmods. Files already as they should be are not
// touched. `progress(done, total)` is called as it goes.
ApplyStats apply_plan(const Plan& plan, const std::string& state_dir, const MergeEnv& env, const LogFn& log,
                      const std::function<void(int, int)>& progress = nullptr);
// Everything the record lists, undone. Returns how many files.
int revert_all(const std::string& state_dir, const LogFn& log);
// The mods the record says are applied, and the order they were applied in.
std::pair<std::vector<std::string>, std::vector<std::string>> applied_info(const std::string& state_dir);
std::string read_merge_log(const std::string& state_dir, std::size_t tail_bytes = 400000);

// ------------------------------------------------------------------ paths
// True when the two paths are the same or one is inside the other.
bool paths_overlap(const std::string& a, const std::string& b);
enum class FolderProblem { None, OverlayEmpty, OverlayInsideOrContainsMods, OverlayInsideOrContainsGame, ModsEmpty };
// Whether the overlay, the mods folder and the game folder can be used together.
FolderProblem check_folders(const std::string& app0, const std::string& overlay, const std::string& mods_dir);
// Is dvdroot_ps4 in `app0`? `message` says what was found.
bool check_bloodborne(const std::string& app0, std::string* message);

// The tables whose original rows are merged cell by cell (the Python merger's
// paramtool/config.txt), lower case, without ".param".
const std::vector<std::string>& param_tables();

}  // namespace mods
