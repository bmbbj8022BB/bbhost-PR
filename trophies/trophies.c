/* trophies: trophy popup (icon card) + chime + in-game trophy list for bbhost (plugin API 29).
 *
 * - Trophy grades, names and descriptions (English and Korean) are read from the game's own trophy00.trp when the
 *   first frame runs (trophy_data.h); nothing of the text is built into the DLL.
 * - Watches <data>/trophies.txt (bbhost writes "id unix-time" per unlock) and, for each new id, shows a popup
 *   (an icon card drawn onto the frame, see trophy_popup.h; the text banner when that is off or unavailable)
 *   and plays a chime. The first scan after start is the baseline: no popups for old unlocks.
 * - The platinum (id 0): if trophies.txt has an id 0 line (the host awards it) that is used; otherwise it counts as
 *   earned once trophies 1-33 are, dated by the latest of those 33 (the moment it really unlocked), so the date is
 *   the same on every start. Nothing is written: the plugin never touches the
 *   host's trophies.txt, and an old <plugin dir>/platinum.txt from an earlier version is only read.
 * - trophies.txt lines are checked one by one ("id unix-time", 0-39, a plausible time); a torn or junk line is
 *   skipped and read again once the file grows, and the file is not re-read while its size and time are unchanged.
 * - An in-game menu page "Trophies" (F10 -> Open Trophies) lists all 40 with grade and date.
 * - Sound: Windows only (winmm PlaySound). A built-in synthesized chime, or your own <plugin dir>/trophy.wav
 *   (16-bit PCM WAV; volume is applied to it). Plugin dir = <data>/plugins/trophies/.
 *
 * - Your own images: <plugin dir>/icons/ (the "Open image folder" button opens it). bb.<image> replaces the BB mark on
 *   the right of the popup and is drawn as it is; trophy00.<image> .. trophy39.<image> replace one trophy's icon.
 *   <image> is png, jpg, bmp, gif, tiff, ico (or webp where Windows has the codec), any size: it is scaled to fit.
 *
 * With bbhost's overlay image (plugin API 30) the popup is drawn over the finished frame: sharp, not blurred. On an
 * older host it is copied onto the picture before the post-processing, as before.
 *
 * Build (Windows DLL, MSYS2 UCRT64; needs: pacman -S mingw-w64-ucrt-x86_64-vulkan-headers):
 *   gcc -shared -O2 -I<bbhost>/include -o trophies.dll trophies.c -lwinmm -lgdi32 -luser32
 * Build (Linux, no sound, no popup):
 *   cc -shared -fPIC -O2 -I<bbhost>/include -o plugins/trophies.so trophies.c -lm
 */
#include "bbhost_plugin.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define N_TROPHIES 40
#define QUEUE_MAX 64
#define SCAN_EVERY_FRAMES 30

static const BbHostApi* api;
static char data_path[1024];     /* <data>/trophies.txt */
static char plat_path[1024];     /* <plugin dir>/platinum.txt */
static char wav_path[1024];      /* <plugin dir>/trophy.wav */
static char mp3_path[1024];  /* <plugin dir>/trophy.mp3, played through MCI */
static int have_mp3;
static char plat_mp3_path[1024];  /* <plugin dir>/trophy_platinum.mp3 */
static int have_plat_mp3;
static char dir_path[1024];      /* <plugin dir>: where trophy.wav / trophy.mp3 / trophy_platinum.mp3 go */

static char known[N_TROPHIES];
static long long when[N_TROPHIES];
static int baseline_done;
static int dirty = 1;            /* the list page must be rebuilt */
static unsigned frames;
static int ov_ok;                /* the host draws images over the finished frame (plugin API 30): trophy_popup.h */
static int (*ov_set_fn)(const uint8_t* rgba, uint32_t w, uint32_t h);
static int (*ov_place_fn)(float x, float y, float w, float h, float alpha);
static int (*ov_area_fn)(uint32_t* w, uint32_t* h);

static int queue[QUEUE_MAX];
static int q_head, q_count;
static time_t next_show;

#include "trophy_data.h"

/* ---------- config helpers (config() pointers are short-lived: copy at once) ---------- */

static int cfg_bool(const char* key, int def) {
    const char* s = api->config(key);
    if (!s || !*s) return def;
    return strcmp(s, "true") == 0 || strcmp(s, "1") == 0;
}
static float cfg_float(const char* key, float def) {
    const char* s = api->config(key);
    if (!s || !*s) return def;
    return (float)atof(s);
}
/* Follows the game's own text language (same globals glyphsearch reads; bbhost 1.09 eboot only):
 * Korean when the first global is 19 (English reads 1), English for anything else or if the read fails. */
#define GAME_LANG_A_BN (0x55992f0ull + 0x400000ull)
#define KO_VALUE_A 19
static int cfg_is_ko(void) {
    int32_t a = -999;
    if (!api->eboot_is_109 || !api->eboot_is_109() || !api->read) return 0;
    if (api->read(GAME_LANG_A_BN, &a, sizeof(a)) != 0) return 0;
    return a == KO_VALUE_A;
}
static int last_ko = -1;

/* ---------- sound ---------- */

static unsigned char* custom_wav;   /* the player's trophy.wav as read, or NULL */
static size_t custom_len;
static unsigned char* play_wav;     /* what is handed to PlaySound (volume applied) */
static size_t play_len;
static float play_vol = -1.0f;

static void put_u16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_u32(unsigned char* p, unsigned v) { put_u16(p, v & 0xffff); put_u16(p + 2, v >> 16); }
static unsigned get_u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

/* A bell-like two-note chime, 44.1 kHz mono 16-bit, ~1.1 s. */
static void synth_chime(float vol) {
    const int rate = 44100;
    const int n = rate * 11 / 10;
    play_len = 44 + (size_t)n * 2;
    play_wav = (unsigned char*)malloc(play_len);
    if (!play_wav) { play_len = 0; return; }
    memcpy(play_wav, "RIFF", 4); put_u32(play_wav + 4, (unsigned)(play_len - 8));
    memcpy(play_wav + 8, "WAVEfmt ", 8); put_u32(play_wav + 16, 16);
    put_u16(play_wav + 20, 1); put_u16(play_wav + 22, 1);
    put_u32(play_wav + 24, rate); put_u32(play_wav + 28, rate * 2);
    put_u16(play_wav + 32, 2); put_u16(play_wav + 34, 16);
    memcpy(play_wav + 36, "data", 4); put_u32(play_wav + 40, (unsigned)n * 2);
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double s = 0;
        /* note 1: E6 from 0 s; note 2: A6 from 0.16 s; each with a soft attack and an exponential decay */
        double a1 = (1.0 - exp(-t * 400.0)) * exp(-t * 4.5);
        s += a1 * (sin(2 * 3.14159265358979 * 1318.5 * t) + 0.3 * sin(2 * 3.14159265358979 * 2637.0 * t));
        if (t > 0.16) {
            double u = t - 0.16;
            double a2 = (1.0 - exp(-u * 400.0)) * exp(-u * 3.6);
            s += 1.1 * a2 * (sin(2 * 3.14159265358979 * 1760.0 * u) + 0.3 * sin(2 * 3.14159265358979 * 3520.0 * u));
        }
        s *= 0.35 * vol;
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        put_u16(play_wav + 44 + (size_t)i * 2, (unsigned)(short)(s * 32767.0) & 0xffff);
    }
}

/* The player's WAV with `vol` applied, when it is 16-bit PCM; otherwise a plain copy (volume not applied). */
static void scale_custom(float vol) {
    play_len = custom_len;
    play_wav = (unsigned char*)malloc(play_len);
    if (!play_wav) { play_len = 0; return; }
    memcpy(play_wav, custom_wav, custom_len);
    if (custom_len < 44 || memcmp(play_wav, "RIFF", 4) || memcmp(play_wav + 8, "WAVE", 4)) return;
    int pcm16 = 0;
    size_t pos = 12;
    while (pos + 8 <= custom_len) {
        unsigned sz = get_u32(play_wav + pos + 4);
        const unsigned char* body = play_wav + pos + 8;
        if (!memcmp(play_wav + pos, "fmt ", 4) && sz >= 16 && pos + 8 + 16 <= custom_len)
            pcm16 = (body[0] | (body[1] << 8)) == 1 && (body[14] | (body[15] << 8)) == 16;
        if (!memcmp(play_wav + pos, "data", 4)) {
            if (!pcm16) return;
            size_t avail = custom_len - (pos + 8);
            size_t len = sz < avail ? sz : avail;
            for (size_t i = 0; i + 1 < len; i += 2) {
                short v = (short)(body[i] | (body[i + 1] << 8));
                int w = (int)(v * vol);
                if (w > 32767) w = 32767;
                if (w < -32768) w = -32768;
                put_u16(play_wav + pos + 8 + i, (unsigned)(short)w & 0xffff);
            }
            return;
        }
        pos += 8 + sz + (sz & 1);
    }
}

static void load_custom_wav(void) {
    {
        FILE* m = fopen(mp3_path, "rb");
        if (m) { fclose(m); have_mp3 = 1; api->log("trophies: using %s", mp3_path); }
        FILE* pm = fopen(plat_mp3_path, "rb");
        if (pm) { fclose(pm); have_plat_mp3 = 1; api->log("trophies: platinum uses %s", plat_mp3_path); }
    }
    FILE* f = fopen(wav_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 44 && n < 8 * 1024 * 1024) {
        custom_wav = (unsigned char*)malloc((size_t)n);
        if (custom_wav && fread(custom_wav, 1, (size_t)n, f) == (size_t)n) custom_len = (size_t)n;
        else { free(custom_wav); custom_wav = NULL; }
    }
    fclose(f);
    if (custom_wav) api->log("trophies: using %s", wav_path);
}

#ifdef _WIN32
static void play_mp3(const char* path, float vol) {
    wchar_t wpath[1024], cmd[1100];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024);
    for (wchar_t* c = wpath; *c; c++) if (*c == L'/') *c = L'\\';
    mciSendStringW(L"close trophy_snd", NULL, 0, NULL);
    wsprintfW(cmd, L"open \"%s\" type mpegvideo alias trophy_snd", wpath);
    if (mciSendStringW(cmd, NULL, 0, NULL)) return;
    wsprintfW(cmd, L"setaudio trophy_snd volume to %d", (int)(vol * 1000.0f));
    mciSendStringW(cmd, NULL, 0, NULL);
    mciSendStringW(L"play trophy_snd from 0", NULL, 0, NULL);
}
#endif

static void play_chime(int plat) {
    float vol = cfg_float("trophies.volume", 0.7f);
    if (vol < 0) vol = 0;
    if (vol > 1) vol = 1;
#ifdef _WIN32
    if (plat && have_plat_mp3) { play_mp3(plat_mp3_path, vol); return; }
    if (have_mp3) { play_mp3(mp3_path, vol); return; }
    if (!play_wav || vol != play_vol) {
        PlaySoundW(NULL, NULL, 0); /* stop before the buffer goes */
        free(play_wav);
        play_wav = NULL;
        if (custom_wav) scale_custom(vol); else synth_chime(vol);
        play_vol = vol;
    }
    if (play_wav) PlaySoundW((LPCWSTR)play_wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#else
    (void)vol; (void)plat; /* no sound off Windows */
#endif
}

/* ---------- unlock state ---------- */

static const char* grade_name(char g, int ko) {
    switch (g) {
    case 'P': return ko ? "플래티넘" : "Platinum";
    case 'G': return ko ? "금" : "Gold";
    case 'S': return ko ? "은" : "Silver";
    default: return ko ? "동" : "Bronze";
    }
}

static void push_new(int id) {
    if (q_count >= QUEUE_MAX) return;
    queue[(q_head + q_count) % QUEUE_MAX] = id;
    q_count++;
}

/* One trophies.txt line: "<id> <unix time>" and nothing else. Anything torn (a line the host is still writing),
 * a comment or a stray value is refused; a truncated time is always below 1e9, so it cannot pass as a real one. */
static int parse_line(const char* s, int* id, long long* ts) {
    char *e1, *e2;
    long a = strtol(s, &e1, 10);
    if (e1 == s) return 0;
    long long t = strtoll(e1, &e2, 10);
    if (e2 == e1) return 0;
    while (*e2 == ' ' || *e2 == '\t' || *e2 == '\r' || *e2 == '\n') e2++;
    if (*e2) return 0;
    if (a < 0 || a >= N_TROPHIES) return 0;
    if (t < 1000000000LL || t > 253402300799LL) return 0;
    *id = (int)a;
    *ts = t;
    return 1;
}

static long long seen_size = -1, seen_mtime = -1;

/* Reads trophies.txt (only when its size or time changed); every id not seen before is marked (and queued once
 * the baseline is in). */
static void scan(void) {
    struct stat st;
    if (stat(data_path, &st) == 0 &&
        (!baseline_done || (long long)st.st_size != seen_size || (long long)st.st_mtime != seen_mtime)) {
        seen_size = (long long)st.st_size;
        seen_mtime = (long long)st.st_mtime;
        FILE* f = fopen(data_path, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                int id;
                long long ts;
                if (!parse_line(line, &id, &ts) || known[id]) continue;
                known[id] = 1;
                when[id] = ts;
                dirty = 1;
                if (baseline_done) push_new(id);
            }
            fclose(f);
        }
    }
    /* The platinum, if the host did not write it: every base-game trophy (1-33), dated by the latest of them. */
    if (!known[0]) {
        int all = 1;
        long long latest = 0;
        for (int i = 1; i <= 33; i++) {
            if (!known[i]) { all = 0; break; }
            if (when[i] > latest) latest = when[i];
        }
        if (all) {
            known[0] = 1;
            when[0] = latest;
            dirty = 1;
            if (baseline_done) push_new(0);
        }
    }
    baseline_done = 1;
}

#include "trophy_popup.h"

/* ---------- popup / banner ---------- */

static void announce(int id) {
    char text[512];
    int ko = cfg_is_ko();
    const TrophyInfo* t = &TROPHIES[id];
    int bko = 0; /* show_message's overlay font has no Hangul: the text banner is English */
    snprintf(text, sizeof(text), "%s  [%s] %s", bko ? "트로피 획득" : "Trophy earned", grade_name(t->grade, bko),
             bko ? t->ko_name : t->en_name);
    float sec = cfg_float("trophies.seconds", 5.0f);
    if (sec < 2) sec = 2;
    if (sec > 15) sec = 15;
    int shown = cfg_bool("trophies.popup", 1) && start_popup(id, ko, sec);
    if (!shown && cfg_bool("trophies.notify", 1)) api->show_message(text, sec);
    if (shown || cfg_bool("trophies.notify", 1)) next_show = time(NULL) + (time_t)sec + 1;
    if (cfg_bool("trophies.sound", 1)) play_chime(t->grade == 'P');
    api->log("trophies: unlocked %d (%s)", id, t->en_name);
}

/* ---------- the list page ---------- */

#define HDR 3 /* progress, base game, The Old Hunters */
static BbOption rows[N_TROPHIES + HDR];
static char lab[N_TROPHIES + HDR][200];
static char val[N_TROPHIES + HDR][64];
static char hlp[N_TROPHIES + HDR][300];
static char key[N_TROPHIES + HDR][12];

static void rebuild(void) {
    int ko = cfg_is_ko(); /* the in-game menu's own font shows Hangul fine */
    int done = 0, base = 0, dlc = 0;
    for (int i = 0; i < N_TROPHIES; i++) {
        done += known[i];
        if (i < 34) base += known[i]; else dlc += known[i];
    }

    snprintf(key[0], sizeof(key[0]), "progress");
    snprintf(lab[0], sizeof(lab[0]), "%s", ko ? "달성도" : "Progress");
    snprintf(val[0], sizeof(val[0]), "%d / %d (%d%%)", done, N_TROPHIES, done * 100 / N_TROPHIES);
    snprintf(hlp[0], sizeof(hlp[0]), "%s", ko ? "달성한 트로피 수. 플래티넘은 기본 게임 트로피 33개를 모두 달성하면 받으며, DLC는 필요 없다."
                                              : "Trophies unlocked. The platinum needs all 33 base-game trophies; the DLC is not required.");
    rows[0] = (BbOption){BB_OPT_INFO, key[0], lab[0], hlp[0], val[0], 0, 0, 0};

    snprintf(key[1], sizeof(key[1]), "base");
    snprintf(lab[1], sizeof(lab[1]), "%s", ko ? "기본 게임" : "Base game");
    snprintf(val[1], sizeof(val[1]), "%d / 34", base);
    snprintf(hlp[1], sizeof(hlp[1]), "%s", ko ? "플래티넘 포함 기본 게임 트로피." : "Base-game trophies, platinum included.");
    rows[1] = (BbOption){BB_OPT_INFO, key[1], lab[1], hlp[1], val[1], 0, 0, 0};

    snprintf(key[2], sizeof(key[2]), "dlc");
    snprintf(lab[2], sizeof(lab[2]), "%s", ko ? "올드 헌터즈" : "The Old Hunters");
    snprintf(val[2], sizeof(val[2]), "%d / 6", dlc);
    snprintf(hlp[2], sizeof(hlp[2]), "%s", ko ? "확장팩 트로피." : "Expansion trophies.");
    rows[2] = (BbOption){BB_OPT_INFO, key[2], lab[2], hlp[2], val[2], 0, 0, 0};

    for (int i = 0; i < N_TROPHIES; i++) {
        const TrophyInfo* t = &TROPHIES[i];
        int r = i + HDR;
        snprintf(key[r], sizeof(key[r]), "t%d", i);
        snprintf(lab[r], sizeof(lab[r]), "[%s] %s", grade_name(t->grade, ko), ko ? t->ko_name : t->en_name);
        snprintf(hlp[r], sizeof(hlp[r]), "%s", ko ? t->ko_desc : t->en_desc);
        if (known[i]) {
            char d[32] = "";
            time_t ts = (time_t)when[i];
            struct tm* tm = ts > 0 ? localtime(&ts) : NULL;
            if (tm) strftime(d, sizeof(d), "%Y-%m-%d %H:%M", tm);
            snprintf(val[r], sizeof(val[r]), "%s %s", ko ? "달성" : "Unlocked", d);
        } else {
            snprintf(val[r], sizeof(val[r]), "%s", ko ? "미달성" : "Locked");
        }
        rows[r] = (BbOption){BB_OPT_INFO, key[r], lab[r], hlp[r], val[r], 0, 0, 0};
    }
    api->panel_rows("trophies", "list", rows, N_TROPHIES + HDR);
    dirty = 0;
}

/* ---------- callbacks ---------- */

/* The trophy text from the game's trophy00.trp, once (the list is rebuilt with it). */
static void ensure_trophy_data(void) {
    if (td_state) return;
    td_state = td_load() ? 1 : -1;
    dirty = 1;
}

static void on_frame(void* user) {
    (void)user;
    frames++;
    ensure_trophy_data();
    { int k = cfg_is_ko(); if (k != last_ko) { last_ko = k; dirty = 1; } } /* game language changed */
    overlay_tick();
    if (frames % SCAN_EVERY_FRAMES == 1) scan();
    if (dirty) rebuild();
    if (q_count > 0 && time(NULL) >= next_show) {
        int id = queue[q_head];
        q_head = (q_head + 1) % QUEUE_MAX;
        q_count--;
        announce(id);
    }
}

static void on_option(const char* k, const char* v, void* user) {
    (void)k; (void)v; (void)user;
    dirty = 1; /* language may have changed */
}

/* Opens `dir` (made first if it is not there yet) in the file manager. Shell32 is bound at run time so the build line
 * stays the same. Windows only; on other hosts the click only logs the path. */
static void open_folder(const char* dir) {
    if (!dir || !dir[0]) return;
#ifdef _WIN32
    char p[1100];
    snprintf(p, sizeof(p), "%s", dir);
    for (char* c = p; *c; c++)
        if (*c == '/') *c = '\\';
    CreateDirectoryA(p, NULL); /* harmless if it exists */
    typedef HINSTANCE(WINAPI * ShellExecuteAFn)(HWND, const char*, const char*, const char*, const char*, int);
    HMODULE sh = LoadLibraryA("shell32.dll");
    ShellExecuteAFn fn = sh ? (ShellExecuteAFn)(void*)GetProcAddress(sh, "ShellExecuteA") : NULL;
    if (fn && (INT_PTR)fn(NULL, "open", p, NULL, NULL, SW_SHOWNORMAL) > 32) api->log("trophies: opened %s", p);
    else api->log("trophies: could not open %s", p);
    if (sh) FreeLibrary(sh);
#else
    api->log("trophies: folder is %s", dir);
#endif
}

/* The plugin folder: where trophy.wav / trophy.mp3 / trophy_platinum.mp3 are read from. */
static void open_sound_folder(void) { open_folder(dir_path); }

/* <plugin dir>/icons: the player's own bb.<image> and trophyNN.<image> (see trophy_popup.h). */
static void open_icons_folder(void) {
    char p[1100];
    if (!dir_path[0]) return;
    snprintf(p, sizeof(p), "%s/icons", dir_path);
    open_folder(p);
}

static void on_action(const char* action, void* user) {
    (void)user;
    if (strcmp(action, "open_sound") == 0) { open_sound_folder(); return; }
    if (strcmp(action, "open_icons") == 0) { open_icons_folder(); return; }
    if (strcmp(action, "test") == 0) push_new(21); /* Cleric Beast */
    else if (strcmp(action, "test_plat") == 0) push_new(0);
}

/* ---------- plugin entry ---------- */

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "trophies", "Trophies", "0.6.0", "community",
    "Trophy banner and chime when one unlocks, and an in-game trophy list."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_BOOL | BB_OPT_LIVE, "popup", "Trophy popup", "Show the icon popup when a trophy unlocks (Windows).", "true"},
    {BB_OPT_CHOICE | BB_OPT_LIVE, "position", "Popup position", "Where the popup slides in from the left edge.", "tl", 0, 0, "tl|bl"},
    {BB_OPT_FLOAT | BB_OPT_LIVE, "brightness", "Popup brightness", "Dim the popup (1 is as designed).", "1.0", 0.2, 1.0},
    {BB_OPT_FLOAT | BB_OPT_LIVE, "gain", "Popup gain (old host)", "Only for a host without API 30, where the popup is drawn before the post-processing.", "0.35", 0.05, 4.0},
    {BB_OPT_BOOL | BB_OPT_LIVE, "notify", "Trophy banner", "Show the text banner when the popup is off or unavailable.", "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "sound", "Trophy sound", "Play a chime when a trophy unlocks (Windows).", "true"},
    {BB_OPT_FLOAT | BB_OPT_LIVE, "volume", "Chime volume", "Volume of the chime.", "0.7", 0.0, 1.0},
    {BB_OPT_FLOAT | BB_OPT_LIVE, "seconds", "Banner seconds", "How long the banner stays.", "5", 2.0, 15.0},
    {BB_OPT_ACTION, "test", "Test banner and sound", "Shows the Cleric Beast trophy as if just unlocked."},
    {BB_OPT_ACTION, "test_plat", "Test platinum", "Shows the platinum as if just unlocked."},
    {BB_OPT_ACTION, "open_sound", "Open sound folder", "Opens the folder for trophy.wav, trophy.mp3 and trophy_platinum.mp3."},
    {BB_OPT_ACTION, "open_icons", "Open image folder",
     "Opens the icons folder. bb.png (or .jpg, .bmp ...) replaces the popup's right mark; trophy00.png to trophy39.png "
     "replace a trophy's icon. Any size or image type; it is scaled to fit."},
    {BB_OPT_HEADING | BB_OPT_PANEL | BB_OPT_MENU, "list", "Trophies", "Every trophy and when it was unlocked.",
     "Trophies"},
    {0}};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* host) {
    /* panel_rows arrived in 29; overlay_image_* in 30 (without it the popup is drawn before the post-processing) */
    if (host->version < 29 || host->size < offsetof(BbHostApi, panel_open) + sizeof(host->panel_open)) return 1;
    api = host;
    /* The API 30 overlay calls are read by offset (they follow the last API 29 member), so this one dll is built
     * against either header: sharp popup on a host that has them, the old way on one that does not. */
    ov_ok = 0;
    {
        size_t base = offsetof(BbHostApi, cutscene) + sizeof(host->cutscene);
        if (host->version >= 30 && host->size >= base + 3 * sizeof(void*)) {
            const char* ext = (const char*)host + base;
            memcpy(&ov_set_fn, ext, sizeof(void*));
            memcpy(&ov_place_fn, ext + sizeof(void*), sizeof(void*));
            memcpy(&ov_area_fn, ext + 2 * sizeof(void*), sizeof(void*));
            ov_ok = ov_set_fn && ov_place_fn && ov_area_fn;
        }
    }

    snprintf(data_path, sizeof(data_path), "%s/trophies.txt", api->data_dir());
    const char* pd = api->plugin_dir("trophies");
    snprintf(dir_path, sizeof(dir_path), "%s", pd);
    snprintf(plat_path, sizeof(plat_path), "%s/platinum.txt", pd);
    snprintf(wav_path, sizeof(wav_path), "%s/trophy.wav", pd);
    snprintf(mp3_path, sizeof(mp3_path), "%s/trophy.mp3", pd);
    snprintf(plat_mp3_path, sizeof(plat_mp3_path), "%s/trophy_platinum.mp3", pd);

    FILE* f = fopen(plat_path, "r");
    if (f) {
        long long ts = 0;
        if (fscanf(f, "%lld", &ts) == 1) { known[0] = 1; when[0] = ts; }
        fclose(f);
    }
    load_custom_wav();

    if (!ov_ok) api->on_render("trophies", BB_RENDER_AFTER_UPSCALE, on_render, 0);
    api->on_frame(on_frame, 0);
    api->on_option("trophies", on_option, 0);
    api->on_action("trophies", on_action, 0);
    api->log("trophies: watching %s (popup %s)", data_path, ov_ok ? "over the finished frame" : "before the post-processing");
    return 0;
}
