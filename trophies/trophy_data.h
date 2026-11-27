/* trophy_data.h: trophy grades, names and descriptions, read from the game's own trophy00.trp at run time.
 *
 * The TRP holds one encrypted XML (ESFM) per language: TROP_01.ESFM is English (US), TROP_09.ESFM Korean. Each is
 * AES-128-CBC:  file key = AES-128-encrypt(trophy key, NP communication id padded to 16 bytes with zeros), IV = the
 * first 16 bytes of the entry, the rest the ciphertext (PKCS#7 padded). The communication id (NPWRnnnnn_00) is read
 * from /sce_sys/npbind.dat. The AES is written out here, so nothing extra has to be linked.
 *
 * The trophy key is built in below; a file <plugin dir>/trophy_key.txt with 32 hex digits replaces it.
 * Without readable game files every trophy is just "Trophy N" (the popup and the list still work).
 *
 * Included by trophies.c after N_TROPHIES and `api` are declared.
 */
#ifndef TROPHY_DATA_H
#define TROPHY_DATA_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char grade; char en_name[96], en_desc[256], ko_name[96], ko_desc[256]; } TrophyInfo;
static TrophyInfo TROPHIES[N_TROPHIES];
static int td_state; /* 0 not tried, 1 loaded, -1 failed */

/* ---------- AES-128 (encrypt one block, decrypt one block, CBC decrypt) ---------- */

static uint8_t td_sb[256], td_isb[256];
typedef struct { uint8_t rk[176]; } TdAes;

static uint8_t td_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t td_rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }
static uint8_t td_mul(uint8_t x, uint8_t y) {
    uint8_t r = 0;
    while (y) {
        if (y & 1) r ^= x;
        x = td_xt(x);
        y >>= 1;
    }
    return r;
}

static void td_tables(void) {
    static int done;
    if (done) return;
    done = 1;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0)); /* p * 3 */
        q ^= (uint8_t)(q << 1);                                         /* q / 3 */
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ td_rotl8(q, 1) ^ td_rotl8(q, 2) ^ td_rotl8(q, 3) ^ td_rotl8(q, 4));
        td_sb[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    td_sb[0] = 0x63;
    for (int i = 0; i < 256; i++) td_isb[td_sb[i]] = (uint8_t)i;
}

static void td_key(TdAes* a, const uint8_t* key) {
    td_tables();
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(td_sb[t[1]] ^ rcon);
            t[1] = td_sb[t[2]];
            t[2] = td_sb[t[3]];
            t[3] = td_sb[u];
            rcon = td_xt(rcon);
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = (uint8_t)(a->rk[i - 16 + j] ^ t[j]);
    }
}

static void td_addrk(uint8_t* s, const uint8_t* k) {
    for (int i = 0; i < 16; i++) s[i] ^= k[i];
}

/* state bytes are column-major: s[col * 4 + row], as the block is laid out */
static void td_enc_block(const TdAes* a, uint8_t* s) {
    td_addrk(s, a->rk);
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) s[i] = td_sb[s[i]];
        for (int c = 0; c < 4; c++)
            for (int w = 0; w < 4; w++) t[c * 4 + w] = s[((c + w) & 3) * 4 + w];
        memcpy(s, t, 16);
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                s[c * 4 + 0] = (uint8_t)(td_xt(a0) ^ td_xt(a1) ^ a1 ^ a2 ^ a3);
                s[c * 4 + 1] = (uint8_t)(a0 ^ td_xt(a1) ^ td_xt(a2) ^ a2 ^ a3);
                s[c * 4 + 2] = (uint8_t)(a0 ^ a1 ^ td_xt(a2) ^ td_xt(a3) ^ a3);
                s[c * 4 + 3] = (uint8_t)(td_xt(a0) ^ a0 ^ a1 ^ a2 ^ td_xt(a3));
            }
        }
        td_addrk(s, a->rk + 16 * r);
    }
}

static void td_dec_block(const TdAes* a, uint8_t* s) {
    td_addrk(s, a->rk + 160);
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int w = 0; w < 4; w++) t[((c + w) & 3) * 4 + w] = s[c * 4 + w];
        for (int i = 0; i < 16; i++) s[i] = td_isb[t[i]];
        td_addrk(s, a->rk + 16 * r);
        if (r > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                s[c * 4 + 0] = (uint8_t)(td_mul(a0, 14) ^ td_mul(a1, 11) ^ td_mul(a2, 13) ^ td_mul(a3, 9));
                s[c * 4 + 1] = (uint8_t)(td_mul(a0, 9) ^ td_mul(a1, 14) ^ td_mul(a2, 11) ^ td_mul(a3, 13));
                s[c * 4 + 2] = (uint8_t)(td_mul(a0, 13) ^ td_mul(a1, 9) ^ td_mul(a2, 14) ^ td_mul(a3, 11));
                s[c * 4 + 3] = (uint8_t)(td_mul(a0, 11) ^ td_mul(a1, 13) ^ td_mul(a2, 9) ^ td_mul(a3, 14));
            }
        }
    }
}

/* ---------- the TRP and its XML ---------- */

static uint32_t td_be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Entry `name` of a TRP (header 0xDCA24D00; count at 16, stride at 20, entries from 96: name[32], offset u64, size u64,
 * all big-endian): its bytes, 1 when found and inside the file. */
static int td_trp_entry(const uint8_t* d, size_t n, const char* name, const uint8_t** out, size_t* len) {
    if (n < 96 || td_be32(d) != 0xDCA24D00u) return 0;
    uint32_t count = td_be32(d + 16), stride = td_be32(d + 20);
    if (stride < 48) return 0;
    for (uint32_t i = 0; i < count && 96 + (size_t)(i + 1) * stride <= n; i++) {
        const uint8_t* e = d + 96 + (size_t)i * stride;
        if (strncmp((const char*)e, name, 32) != 0) continue;
        uint64_t off = ((uint64_t)td_be32(e + 32) << 32) | td_be32(e + 36);
        uint64_t sz = ((uint64_t)td_be32(e + 40) << 32) | td_be32(e + 44);
        if (off > n || sz > n - off) return 0;
        *out = d + off;
        *len = (size_t)sz;
        return 1;
    }
    return 0;
}

/* One ESFM entry decrypted: malloc'd, NUL-terminated XML text, or NULL when the key does not fit it. */
static char* td_esfm(const uint8_t* blob, size_t len, const TdAes* key) {
    if (len < 32) return NULL;
    size_t n = (len - 16) / 16 * 16;
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    uint8_t prev[16], cur[16];
    memcpy(prev, blob, 16);
    for (size_t i = 0; i < n; i += 16) {
        memcpy(cur, blob + 16 + i, 16);
        uint8_t b[16];
        memcpy(b, cur, 16);
        td_dec_block(key, b);
        for (int k = 0; k < 16; k++) out[i + k] = (char)(b[k] ^ prev[k]);
        memcpy(prev, cur, 16);
    }
    unsigned pad = (uint8_t)out[n - 1];
    if (pad >= 1 && pad <= 16 && pad <= n) n -= pad;
    out[n] = 0;
    if (!strstr(out, "<trophyconf")) {
        free(out);
        return NULL;
    }
    return out;
}

/* [s, e) as text into dst: XML entities decoded, line breaks and runs of blanks become one space, ends trimmed. */
static void td_text(const char* s, const char* e, char* dst, size_t cap) {
    size_t o = 0;
    int space = 0;
    while (s < e && o + 1 < cap) {
        unsigned c = (uint8_t)*s;
        if (c == '&') {
            const char* semi = (const char*)memchr(s, ';', (size_t)(e - s));
            if (semi && semi - s <= 10) {
                if (s[1] == '#') {
                    c = (unsigned)(s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10));
                } else if (!strncmp(s, "&amp;", 5)) c = '&';
                else if (!strncmp(s, "&lt;", 4)) c = '<';
                else if (!strncmp(s, "&gt;", 4)) c = '>';
                else if (!strncmp(s, "&quot;", 6)) c = '"';
                else if (!strncmp(s, "&apos;", 6)) c = '\'';
                s = semi + 1;
                if (c >= 0x80) continue; /* a non-ASCII character reference is not used by the files */
            } else {
                s++;
            }
        } else {
            s++;
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = 1;
            continue;
        }
        if (space && o > 0) dst[o++] = ' ';
        space = 0;
        if (o + 1 < cap) dst[o++] = (char)c;
    }
    dst[o] = 0;
}

/* <trophy id="NNN" ... ttype="X" ...> <name>..</name> <detail>..</detail> </trophy> of one language file into the table
 * (ko = 0: English fields and the grade, 1: Korean fields). Returns how many trophies it found. */
static int td_parse(const char* xml, int ko) {
    int found = 0;
    const char* p = xml;
    while ((p = strstr(p, "<trophy id=\"")) != NULL) {
        p += 12;
        int id = atoi(p);
        const char* tag_end = strchr(p, '>');
        if (!tag_end) break;
        if (tag_end[-1] == '/') { /* <trophy .../>: the id and grade only, no text (TROPCONF.ESFM) */
            p = tag_end + 1;
            continue;
        }
        const char* end = strstr(tag_end, "</trophy>");
        if (!end) break;
        if (id >= 0 && id < N_TROPHIES) {
            const char* tt = strstr(p, "ttype=\"");
            TrophyInfo* t = &TROPHIES[id];
            if (tt && tt < tag_end && !ko) t->grade = tt[7];
            const char* n0 = strstr(tag_end, "<name>");
            const char* n1 = n0 ? strstr(n0, "</name>") : NULL;
            const char* d0 = strstr(tag_end, "<detail>");
            const char* d1 = d0 ? strstr(d0, "</detail>") : NULL;
            if (n0 && n1 && n1 < end) td_text(n0 + 6, n1, ko ? t->ko_name : t->en_name, sizeof(t->en_name));
            if (d0 && d1 && d1 < end) td_text(d0 + 8, d1, ko ? t->ko_desc : t->en_desc, sizeof(t->en_desc));
            found++;
        }
        p = end + 9;
    }
    return found;
}

/* ---------- loading ---------- */

#define TD_DEFAULT_KEY "21F41A6BAD8A1D3ECA7AD586C101B7A9"
#define TD_DEFAULT_COMM_ID "NPWR05818_00"

static int td_hex16(const char* s, uint8_t* out) {
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = *s++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

/* A game file by its /app0 path, from the first of the two usual roots that has it. The host's buffer stays valid only
 * until this thread's next game_file call, so the caller finishes with it first. */
static int td_game_file(const char* rel, const uint8_t** d, size_t* n) {
    static const char* const roots[] = {"", "/dvdroot_ps4"};
    for (int k = 0; k < 2; k++) {
        char path[160];
        snprintf(path, sizeof(path), "%s%s", roots[k], rel);
        const void* data = NULL;
        size_t size = 0;
        if (api->game_file(path, &data, &size) == 0 && data && size) {
            *d = (const uint8_t*)data;
            *n = size;
            return 1;
        }
    }
    return 0;
}

/* The communication id from npbind.dat: the first NPWRnnnnn_nn in it. */
static int td_comm_id(char* out) {
    const uint8_t* d;
    size_t n;
    if (!td_game_file("/sce_sys/npbind.dat", &d, &n)) return 0;
    for (size_t i = 0; i + 12 <= n && i < 0x1000; i++) {
        if (memcmp(d + i, "NPWR", 4) != 0) continue;
        int ok = d[i + 9] == '_';
        for (int k = 4; k < 9 && ok; k++) ok = d[i + k] >= '0' && d[i + k] <= '9';
        for (int k = 10; k < 12 && ok; k++) ok = d[i + k] >= '0' && d[i + k] <= '9';
        if (!ok) continue;
        memcpy(out, d + i, 12);
        out[12] = 0;
        return 1;
    }
    return 0;
}

/* The language file `want` (falling back to `fallback`) decrypted and parsed into the table; 1 when it gave trophies. */
static int td_language(const uint8_t* trp, size_t trp_len, const TdAes* key, const char* want, const char* fallback, int ko) {
    const char* names[2] = {want, fallback};
    for (int i = 0; i < 2; i++) {
        const uint8_t* blob;
        size_t len;
        if (!names[i] || !td_trp_entry(trp, trp_len, names[i], &blob, &len)) continue;
        char* xml = td_esfm(blob, len, key);
        if (!xml) continue;
        int found = td_parse(xml, ko);
        free(xml);
        if (found > 0) {
            api->log("trophies: %s read from %s (%d trophies)", ko ? "Korean text" : "English text", names[i], found);
            return 1;
        }
    }
    return 0;
}

static int td_load(void) {
    memset(TROPHIES, 0, sizeof(TROPHIES));
    for (int i = 0; i < N_TROPHIES; i++) {
        TROPHIES[i].grade = 'B';
        snprintf(TROPHIES[i].en_name, sizeof(TROPHIES[i].en_name), "Trophy %d", i);
        snprintf(TROPHIES[i].ko_name, sizeof(TROPHIES[i].ko_name), "Trophy %d", i);
    }
    if (!api || !api->game_file) return 0;

    uint8_t trophy_key[16];
    td_hex16(TD_DEFAULT_KEY, trophy_key);
    {
        char kp[1100], hex[80] = "";
        snprintf(kp, sizeof(kp), "%s/trophy_key.txt", api->plugin_dir("trophies"));
        FILE* f = fopen(kp, "r");
        if (f) {
            if (fscanf(f, "%64s", hex) == 1) {
                uint8_t k[16];
                if (td_hex16(hex, k) && strlen(hex) == 32) memcpy(trophy_key, k, 16);
                else api->log("trophies: %s is not 32 hex digits, using the built-in key", kp);
            }
            fclose(f);
        }
    }

    char ids[2][16] = {"", TD_DEFAULT_COMM_ID}; /* npbind.dat's first, then the one this game is known to use */
    if (!td_comm_id(ids[0])) ids[0][0] = 0;

    const uint8_t* trp;
    size_t trp_len;
    if (!td_game_file("/sce_sys/trophy/trophy00.trp", &trp, &trp_len)) {
        api->log("trophies: trophy00.trp not found in the game files (trophies are shown as Trophy N)");
        return 0;
    }
    for (int c = 0; c < 2; c++) {
        if (!ids[c][0] || (c == 1 && !strcmp(ids[0], ids[1]))) continue;
        uint8_t block[16] = {0};
        memcpy(block, ids[c], 12);
        TdAes a;
        td_key(&a, trophy_key);
        td_enc_block(&a, block); /* the file key */
        TdAes fk;
        td_key(&fk, block);
        int en = td_language(trp, trp_len, &fk, "TROP_01.ESFM", "TROP.ESFM", 0);
        if (!en) continue;
        if (!td_language(trp, trp_len, &fk, "TROP_09.ESFM", NULL, 1)) {
            /* no Korean in this TRP: the English text stands in for it */
            for (int i = 0; i < N_TROPHIES; i++) {
                memcpy(TROPHIES[i].ko_name, TROPHIES[i].en_name, sizeof(TROPHIES[i].en_name));
                memcpy(TROPHIES[i].ko_desc, TROPHIES[i].en_desc, sizeof(TROPHIES[i].en_desc));
            }
            api->log("trophies: no Korean text in this trophy00.trp, English is used");
        }
        api->log("trophies: trophy text from trophy00.trp (%s)", ids[c]);
        return 1;
    }
    api->log("trophies: could not decrypt trophy00.trp (wrong trophy key? put 32 hex digits in trophy_key.txt)");
    return 0;
}

#endif
