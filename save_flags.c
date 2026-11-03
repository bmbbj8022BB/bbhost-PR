/* save_flags: the Noxde save editor's "Restore Maria's dialogue" and
 * "Enable Doll's lullaby" options, done as a bbhost plugin instead of an
 * edit of the save file.
 *
 * How the editor's offsets map to event flags (checked against the editor's
 * bosses.json and bbhost's boss_rush defeat flags: 22 bosses, all fit):
 *   save offset = 2 + 125 * rank + byte
 * where `rank` is the position of the flag's block among the blocks the flag
 * store holds, sorted by block number (blocks are 1000 flags = 125 bytes),
 * and `byte` is the byte inside the block. Flag id = block * 1000 + byte * 8
 * + bit, the first flag of a byte being its most significant bit.
 *   Maria's dialogue: offsets 1083, 1084 -> rank 8, bytes 81, 82 = 0x00, 0x08
 *   Doll's lullaby:   offsets 6689, 6690 -> rank 53, bytes 62, 63 = 0x08, 0x01
 * The editor writes whole bytes, so this plugin does too: all 8 flags of each
 * byte are set or cleared to match.
 *
 * Rank -> block comes from the live flag store (its ordered map), and is only
 * trusted when known bosses' blocks sit at the ranks the editor's offsets say
 * (anchors below). If they do not, nothing is written and the log says why.
 *
 * Opt-in. Untested in game: use a copy of the save.
 * Build: cc -shared -fPIC -I<bbhost>/include -o save_flags.so save_flags.c */
#include "bbhost_plugin.h"

#include <stdint.h>
#include <string.h>

static const BbHostApi* api;

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, BB_PLUGIN_OPT_IN | BB_PLUGIN_GAMEPLAY, "save_flags", "Save flags", "0.1.0", "unofficial",
    "Turns on Lady Maria's restored dialogue and the Doll's 1.0 lullaby, as the Bloodborne save editor does."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_BOOL, "maria_dialogue", "Restore Maria's dialogue", "Some dialogue before fighting Maria.", "false"},
    {BB_OPT_BOOL, "dolls_lullaby", "Doll's lullaby", "The lullaby from version 1.0.", "false"},
    {BB_OPT_END}};

typedef struct Edit {
    const char* key;
    unsigned rank, byte, value;
} Edit;
/* One entry per byte the editor writes. */
static const Edit edits[] = {
    {"maria_dialogue", 8, 81, 0x00}, {"maria_dialogue", 8, 82, 0x08},
    {"dolls_lullaby", 53, 62, 0x08}, {"dolls_lullaby", 53, 63, 0x01},
};
/* Blocks (flag / 1000) of bosses and the rank the editor's save offsets give them. */
static const struct { uint32_t block; unsigned rank; } anchors[] = {
    {12301, 171}, {12401, 172}, {12411, 173}, {12421, 174}, {12501, 175}, {13501, 183}};

#define MAX_BLOCKS 1024
static uint32_t keys[MAX_BLOCKS];
static unsigned nkeys;
static int pending, done;

static int rd(uint64_t addr, void* out, size_t n) { return api->read(addr, out, n); }

/* In-order walk of the flag store's std::map (flag_store.h: root at head+8,
 * left +0, right +0x10, is-nil byte +0x19, block number +0x20). */
static int load_keys(uint32_t* block_size) {
    uint64_t slot = api->guest_addr(0x593b100), mgr = 0, head = 0, root = 0;
    if (!slot || rd(slot, &mgr, 8) || !mgr) return 1;
    if (rd(mgr + 0x1c, block_size, 4) || rd(mgr + 0x38, &head, 8) || !head) return 1;
    if (rd(head + 8, &root, 8)) return 1;
    uint64_t stack[128];
    int sp = 0;
    uint64_t node = root;
    nkeys = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        uint8_t nil = 1;
        while (node && !rd(node + 0x19, &nil, 1) && !nil) {
            if (sp >= 128) return 1;
            stack[sp++] = node;
            if (rd(node, &node, 8)) return 1;
        }
        if (!sp) break;
        node = stack[--sp];
        uint32_t key = 0;
        if (rd(node + 0x20, &key, 4) || nkeys >= MAX_BLOCKS) return 1;
        keys[nkeys++] = key;
        if (rd(node + 0x10, &node, 8)) return 1;
    }
    return nkeys ? 0 : 1;
}

static int rank_matches_anchors(void) {
    int seen = 0;
    for (unsigned a = 0; a < sizeof(anchors) / sizeof(anchors[0]); ++a) {
        for (unsigned i = 0; i < nkeys; ++i) {
            if (keys[i] != anchors[a].block) continue;
            if (i != anchors[a].rank) {
                api->log("save_flags: block %u is at rank %u, the save offsets say %u", anchors[a].block, i, anchors[a].rank);
                return 0;
            }
            ++seen;
        }
    }
    return seen > 0;
}

static unsigned read_byte(uint32_t base, unsigned byte, int* ok) {
    unsigned v = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        int f = 0;
        if (api->event_flag_get(base + byte * 8 + bit, &f)) *ok = 0;
        v |= (unsigned)(f ? 1 : 0) << (7 - bit);
    }
    return v;
}

static void apply(void) {
    uint32_t size = 0;
    if (load_keys(&size)) { api->log("save_flags: cannot read the flag store yet"); return; }
    if (size != 1000) { api->log("save_flags: block size is %u, not 1000: nothing written", size); return; }
    if (!rank_matches_anchors()) {
        api->log("save_flags: block order does not match the save layout (%u blocks): nothing written", nkeys);
        return;
    }
    done = 1;
    for (unsigned e = 0; e < sizeof(edits) / sizeof(edits[0]); ++e) {
        const char* v = 0;
        char cfg[64];
        strcpy(cfg, "save_flags.");
        strncat(cfg, edits[e].key, 40);
        v = api->config(cfg);
        if (!v || strcmp(v, "true") != 0) continue;
        if (edits[e].rank >= nkeys) { api->log("save_flags: %s: rank %u has no block", edits[e].key, edits[e].rank); continue; }
        const uint32_t base = keys[edits[e].rank] * size;
        int ok = 1;
        const unsigned before = read_byte(base, edits[e].byte, &ok);
        int failed = 0;
        for (unsigned bit = 0; bit < 8; ++bit)
            failed += api->event_flag_set(base + edits[e].byte * 8 + bit, (edits[e].value >> (7 - bit)) & 1) != 0;
        const unsigned after = read_byte(base, edits[e].byte, &ok);
        api->log("save_flags: %s: block %u byte %u (flags %u..%u): %02x -> %02x%s", edits[e].key, keys[edits[e].rank],
                 edits[e].byte, base + edits[e].byte * 8, base + edits[e].byte * 8 + 7, before, after,
                 failed || !ok ? " (some flags could not be written)" : "");
    }
}

static void on_world_load(uint32_t block, void* user) {
    (void)block; (void)user;
    if (!done) pending = 1;
}

/* Flags are written from the main thread, once the character exists. */
static void on_frame(void* user) {
    int64_t level = 0;
    (void)user;
    if (!pending || done) return;
    if (api->player_stat_get("level", &level) || level <= 0) return;
    pending = 0;
    apply();
}

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* a) {
    api = a;
    if (a->version < 8) return 1; /* on_world_load */
    if (!a->eboot_is_109()) { a->log("save_flags: not the 1.09 eboot: not loaded"); return 1; }
    if (a->on_frame(on_frame, 0) || a->on_world_load(on_world_load, 0)) return 1;
    return 0;
}
