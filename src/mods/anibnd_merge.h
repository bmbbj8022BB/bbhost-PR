// Merges several mods' `c0000.anibnd.dcx` into one, against the game's own.
//
// Each mod is compared with the original archive and only what it changed is
// taken:
//  - entries (aNN.tae, c0000.hkx ...) that only one mod changed are taken as
//    they are;
//  - the same aNN.tae changed by several mods is split into its animations
//    and merged one animation at a time;
//  - the same animation (or any other entry) changed by several mods is a
//    conflict: the mod later in the list wins and the report says who lost.
//
// A TAE is rebuilt from the animations' own payloads, so a merged file keeps
// every mod's events and the result is read back and checked before it is
// returned. Nothing here touches the game's files or the mods folder; the
// caller reads the inputs and writes the result.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mods {

struct AnibndMod {
    std::string label;                // shown in the report; later mods win
    std::vector<std::uint8_t> file;   // the mod's file: a DCX, or a bare BND4
};

// Two or more mods changed the same thing.
struct AnibndConflict {
    std::string entry;                // "a000.tae", "Behaviors/c0000.hkx"
    std::int64_t anim = -1;           // the animation id; -1 when the whole entry conflicts
    int index = 0;                    // which of several animations with that id
    std::string winner;
    std::vector<std::string> losers;
};

struct AnibndReport {
    std::vector<AnibndConflict> conflicts;
    // Entries one mod changed alone (the ones nothing else touched).
    struct Taken {
        std::string entry;
        std::vector<std::string> mods;
    };
    std::vector<Taken> taken;
    std::vector<std::string> warnings;
    // How many entries each mod differs from the original in. 0 means the
    // "original" is probably already that mod, and overlaps cannot be seen.
    std::vector<std::pair<std::string, int>> diffs;
    int entries_taken = 0, entries_merged = 0, entries_conflicting = 0;
};

using AnibndLog = std::function<void(const std::string&)>;

// `original` is the game's c0000.anibnd.dcx. With `dry` the result is built
// and checked but not packed (`out` stays empty): for the preview before
// anything is written. False with `error` set when an input cannot be read or
// the merged TAE does not read back as expected.
//
// Almost all the time of a real merge is the zlib pass over the 20+ MB result
// (the merge itself is a fraction of a second), so the level is a parameter:
// the game reads any level, and 6 is several times faster than 9 for a result
// about 1% larger.
bool anibnd_merge(const std::vector<std::uint8_t>& original, const std::vector<AnibndMod>& mods, bool dry,
                  AnibndReport& report, std::vector<std::uint8_t>* out, std::string* error,
                  const AnibndLog& log = nullptr, int compress_level = 6);

}  // namespace mods
