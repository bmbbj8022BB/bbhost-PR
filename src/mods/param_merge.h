// Merges several mods' `gameparam.parambnd.dcx` into one, against the game's own.
//
// Each mod is compared with the original archive, table by table, and only the
// cells it changed are taken (a three-way merge: original / merged so far /
// mod):
//  - a table nobody changed stays as it is;
//  - rows (and cells of a row) that only one mod changed are taken as they are;
//  - a row ID the original does not have is added; two mods adding the same
//    new ID with different values conflict cell by cell;
//  - the same cell set to different values by two mods is a conflict: the mod
//    later in the list wins and the report says who lost.
//
// The cell boundaries come from the game's own `paramdef.paramdefbnd.dcx`
// (the same layout the Python merger reads from its Defs XML). A table that
// cannot be read, or whose merged result does not read back as expected, is
// not dropped: that one table is taken from the later mod as a whole and a
// warning says so. Nothing here touches the game's files or the mods folder;
// the caller reads the inputs and writes the result.
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mods {

struct ParamMod {
    std::string label;                // shown in the report; later mods win
    std::vector<std::uint8_t> file;   // the mod's file: a DCX, or a bare BND4
};

// Two or more mods changed the same thing.
struct ParamConflict {
    std::string table;                // "EquipParamWeapon"
    std::int64_t id = -1;             // the row ID; -1 when the whole table conflicts
    std::vector<std::string> cells;   // the cells that differ (empty for a whole table)
    std::string winner;
    std::vector<std::string> losers;
};

struct ParamReport {
    std::vector<ParamConflict> conflicts;
    // One line per (mod, table) the mod changed in a way that was merged.
    struct Change {
        std::string table;
        std::string mod;
        int modified_rows = 0, new_rows = 0;
    };
    std::vector<Change> changes;
    std::vector<std::string> warnings;
    // How many tables each mod differs from the original in. 0 means the
    // "original" is probably already that mod, and overlaps cannot be seen.
    std::vector<std::pair<std::string, int>> diffs;
    int changed_tables = 0, merged_rows = 0, new_rows = 0, conflict_cells = 0, table_fallbacks = 0;
};

using ParamLog = std::function<void(const std::string&)>;

// `original` is the game's gameparam.parambnd.dcx and `paramdef` its
// paramdef.paramdefbnd.dcx. `allowed_tables` (lower case, without ".param")
// limits which tables get their original rows edited cell by cell; empty means
// every table (the Python merger's config.txt). A table outside that list
// that a mod edited is taken from the later mod as a whole, never silently
// dropped.
//
// With `dry` the result is built and checked but not packed (`out` stays
// empty): for the preview before anything is written. False with `error` set
// when the original or the paramdef cannot be read. As with the animation
// merge, almost all the time of a real merge is the zlib pass over the
// result, so the level is a parameter: the game reads any level, and 6 is
// several times faster than 9 for a result about 1% larger.
bool param_merge(const std::vector<std::uint8_t>& original, const std::vector<std::uint8_t>& paramdef,
                 const std::vector<ParamMod>& mods, const std::set<std::string>& allowed_tables, bool dry,
                 ParamReport& report, std::vector<std::uint8_t>* out, std::string* error,
                 const ParamLog& log = nullptr, int compress_level = 6);

}  // namespace mods
