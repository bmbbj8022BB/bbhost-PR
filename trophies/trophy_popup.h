/* trophy_popup.h: the trophy popup (light card: icon, text, cup, BB mark), rectangular, included by trophies.c.
 *
 * The card is drawn into a CPU bitmap on the main thread (GDI on Windows). With a host that has the overlay image
 * (plugin API 30) it is handed over as an RGBA image and placed each frame: the host draws it over the finished
 * frame, sharp and unblurred (start_overlay_popup, overlay_tick). Otherwise, as before, it is written as RGBA16F into
 * a host-visible staging buffer and the render hook (AFTER_UPSCALE) copies it onto the picture with
 * vkCmdCopyBufferToImage. A copy has no alpha blend, so:
 *   - the rounded shape is made by copying only the pixels inside it, one span per row (hard edge, no fade);
 *   - the card slides in from the left edge and out again (no fade: that needs a compute shader).
 * The hook only records transfer work, as the plugin API requires.
 */
#include "png_lite.h"
#include "trophy_font.h"

#include <stdatomic.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifndef _WIN32
#include <time.h>
#endif

#define POP_CAP_W 1280
#define POP_CAP_H 576
#define POP_SLOT_BYTES ((size_t)POP_CAP_W * POP_CAP_H * 8)
#define POP_BASE_W 580
#define POP_BASE_H 112
#define POP_BAND_H 128 /* the upper band that holds the cup, icon and name */

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* ---------- icons ---------- */

static unsigned char* load_png_path(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (n > 0 && n < 16 * 1024 * 1024) ? (unsigned char*)malloc((size_t)n) : NULL;
    unsigned char* px = NULL;
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) px = png_decode_rgba(b, (size_t)n, w, h);
    free(b);
    fclose(f);
    return px;
}

/* The icon's RGBA8: <plugin dir>/icons/trophyNN.png first, else the game's own trophy00.trp. */
static unsigned char* load_icon(int id, int* w, int* h) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/icons/trophy%02d.png", api->plugin_dir("trophies"), id);
    unsigned char* px = load_png_path(path, w, h);
    if (px) return px;
    static const char* const trp_paths[] = {"/sce_sys/trophy/trophy00.trp", "/dvdroot_ps4/sce_sys/trophy/trophy00.trp"};
    for (int k = 0; k < 2; k++) {
        const void* data = NULL;
        size_t size = 0;
        if (api->game_file(trp_paths[k], &data, &size) != 0 || !data || size < 96) continue;
        const unsigned char* d = (const unsigned char*)data;
        unsigned count = pl_be32(d + 16), stride = pl_be32(d + 20);
        if (pl_be32(d) != 0xDCA24D00u || stride != 64) continue;
        char want[16];
        snprintf(want, sizeof(want), "TROP%03d.PNG", id);
        for (unsigned i = 0; i < count && 96 + (size_t)(i + 1) * 64 <= size; i++) {
            const unsigned char* e = d + 96 + (size_t)i * 64;
            if (strncmp((const char*)e, want, 32) != 0) continue;
            unsigned long long off = ((unsigned long long)pl_be32(e + 32) << 32) | pl_be32(e + 36);
            unsigned long long len = ((unsigned long long)pl_be32(e + 40) << 32) | pl_be32(e + 44);
            if (off + len > size) break;
            return png_decode_rgba(d + off, (size_t)len, w, h);
        }
    }
    return NULL;
}

/* ---------- the card ---------- */

static void grade_color(char g, unsigned char* c) {
    switch (g) {
    case 'P': c[0] = 140; c[1] = 200; c[2] = 255; break;
    case 'G': c[0] = 255; c[1] = 200; c[2] = 60; break;
    case 'S': c[0] = 200; c[1] = 205; c[2] = 215; break;
    default: c[0] = 205; c[1] = 127; c[2] = 50; break;
    }
}

static void sample_bilinear(const unsigned char* src, int w, int h, float fx, float fy, float* o) {
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > (float)(w - 1)) fx = (float)(w - 1);
    if (fy > (float)(h - 1)) fy = (float)(h - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    float tx = fx - (float)x0, ty = fy - (float)y0;
    for (int c = 0; c < 4; c++) {
        float a = src[((size_t)y0 * w + x0) * 4 + c] * (1 - tx) + src[((size_t)y0 * w + x1) * 4 + c] * tx;
        float b = src[((size_t)y1 * w + x0) * 4 + c] * (1 - tx) + src[((size_t)y1 * w + x1) * 4 + c] * tx;
        o[c] = a * (1 - ty) + b * ty;
    }
}

/* Coverage (0..1) of a rounded box [0,w]x[0,h] with corner radius r, at the point (x,y). */
static float rr_cov(float x, float y, float w, float h, float r) {
    float cx = x < r ? r : (x > w - r ? w - r : x);
    float cy = y < r ? r : (y > h - r ? h - r : y);
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
    float c = 0.5f - d;
    return c < 0 ? 0 : (c > 1 ? 1 : c);
}

static void put_mix(unsigned char* p, const float* rgb, float a) {
    for (int k = 0; k < 3; k++) p[k] = (unsigned char)((float)p[k] * (1 - a) + rgb[k] * a + 0.5f);
}

/* Draws img (iw x ih) scaled into the box (dx,dy,dw,dh), with rounded corners of radius r and a border of bw pixels. */
static void blit_scaled(unsigned char* px, int cw, int ch, const unsigned char* img, int iw, int ih, int dx, int dy,
                        int dw, int dh, float r, float bw, const float* bcol) {
    int pad = (int)(bw + 1.5f);
    for (int y = -pad; y < dh + pad; y++)
        for (int x = -pad; x < dw + pad; x++) {
            int X = dx + x, Y = dy + y;
            if (X < 0 || Y < 0 || X >= cw || Y >= ch) continue;
            unsigned char* p = px + ((size_t)Y * cw + X) * 4;
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            if (bw > 0) {
                float oc = rr_cov(fx + bw, fy + bw, (float)dw + 2 * bw, (float)dh + 2 * bw, r + bw);
                if (oc > 0) put_mix(p, bcol, oc);
            }
            if (x < 0 || y < 0 || x >= dw || y >= dh) continue;
            float cov = rr_cov(fx, fy, (float)dw, (float)dh, r);
            if (cov <= 0) continue;
            float c[4] = {0, 0, 0, 255};
            if (img && iw > 0 && ih > 0)
                sample_bilinear(img, iw, ih, fx * (float)iw / (float)dw - 0.5f, fy * (float)ih / (float)dh - 0.5f, c);
            else { c[0] = c[1] = c[2] = 40; }
            float rgb[3] = {c[0], c[1], c[2]};
            put_mix(p, rgb, cov * c[3] / 255.0f);
        }
}

/* The trophy cup, in the grade color. u: -0.5..0.5 across, v: 0 (top) .. 1 (bottom). */
static int cup_in(float u, float v) {
    float au = fabsf(u);
    if (v >= 0.0f && v <= 0.07f && au <= 0.33f) return 1;                  /* rim */
    if (v >= 0.05f && v <= 0.52f) {                                         /* bowl */
        float a = au / 0.30f, b = (v - 0.05f) / 0.47f;
        if (a * a + b * b <= 1.0f) return 1;
    }
    {                                                                       /* handles */
        float du = au - 0.33f, dv = v - 0.20f, d = sqrtf(du * du + dv * dv);
        if (au >= 0.27f && d <= 0.17f && d >= 0.095f) return 1;
    }
    if (au <= 0.055f && v >= 0.5f && v <= 0.80f) return 1;                 /* stem */
    if (au <= 0.21f && v >= 0.80f && v <= 0.95f) return 1;                 /* base */
    return 0;
}

static void draw_cup(unsigned char* px, int cw, int ch, float cx, float top, float size, const unsigned char* acc) {
    int x0 = (int)(cx - 0.6f * size), x1 = (int)(cx + 0.6f * size) + 1;
    int y0 = (int)top - 1, y1 = (int)(top + size) + 2;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (x < 0 || y < 0 || x >= cw || y >= ch) continue;
            int hit = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float u = ((float)x + ((float)sx + 0.5f) / 3.0f - cx) / size;
                    float v = ((float)y + ((float)sy + 0.5f) / 3.0f - top) / size;
                    hit += cup_in(u, v);
                }
            if (!hit) continue;
            float v = ((float)y - top) / size;
            float k = 1.12f - 0.35f * v;
            float rgb[3];
            for (int c = 0; c < 3; c++) { rgb[c] = (float)acc[c] * k; if (rgb[c] > 255) rgb[c] = 255; }
            put_mix(px + ((size_t)y * cw + x) * 4, rgb, (float)hit / 9.0f);
        }
}

/* Light card: trophy icon on the left, small cup, a black circle with a blood-red ring on the right (the "BB" letters
 * are drawn into it afterwards by draw_bb_mark), RGBA8. Returns the x where the text starts. */
static int compose_base(unsigned char* px, int cw, int ch, float s, const unsigned char* acc, const unsigned char* icon,
                        int iw, int ih) {
    for (size_t i = 0; i < (size_t)cw * ch; i++) {
        px[i * 4 + 0] = 244;
        px[i * 4 + 1] = 244;
        px[i * 4 + 2] = 246;
        px[i * 4 + 3] = 255;
    }
    int isz = (int)(80 * s + 0.5f);
    float bcol[3] = {150, 152, 160};
    blit_scaled(px, cw, ch, icon, iw, ih, (int)(18 * s + 0.5f), (ch - isz) / 2, isz, isz, 3 * s, 1.5f * s, bcol);

    unsigned char cc[3]; /* the cup's color: darker so that it shows on the light card */
    for (int k = 0; k < 3; k++) cc[k] = (unsigned char)(acc[k] * 0.82f);
    if (acc[0] == 200 && acc[1] == 205) { cc[0] = 140; cc[1] = 145; cc[2] = 158; } /* silver */
    draw_cup(px, cw, ch, 135 * s, 63 * s, 26 * s, cc);

    /* the black circle at the right */
    float dia = 64 * s, ccx = (float)cw - 56 * s, ccy = (float)ch / 2;
    float black[3] = {14, 12, 14}, ring[3] = {128, 18, 22};
    for (int y = (int)(ccy - dia / 2) - 1; y < (int)(ccy + dia / 2) + 2; y++)
        for (int x = (int)(ccx - dia / 2) - 1; x < (int)(ccx + dia / 2) + 2; x++) {
            if (x < 0 || y < 0 || x >= cw || y >= ch) continue;
            float dx = (float)x + 0.5f - ccx, dy = (float)y + 0.5f - ccy;
            float d = sqrtf(dx * dx + dy * dy);
            float c = 0.5f - (d - dia / 2);
            if (c <= 0) continue;
            unsigned char* p = px + ((size_t)y * cw + x) * 4;
            put_mix(p, black, c > 1 ? 1 : c);
            /* thin blood-red ring just inside the edge */
            float rc = 0.5f - (fabsf(d - (dia / 2 - 3.2f * s)) - 0.7f * s);
            if (rc > 0) put_mix(p, ring, rc > 1 ? 1 : rc);
        }

    return (int)(120 * s + 0.5f);
}

/* Row spans of the card: pixels [0, x1[r]) of row r are copied (here every row is the full width). */
static void compute_spans(short* x1, int cw, int ch) {
    for (int r = 0; r < ch; r++) x1[r] = (short)cw; /* a plain rectangle */
}

#ifdef _WIN32
static void to_wide(const char* u8, wchar_t* w, int cap) {
    if (MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, cap) == 0) w[0] = 0;
}

/* The "fleshandblood" font is loaded from the copy embedded in the DLL, for this process only. */
static const wchar_t* bb_font_name(void) {
    static int tried, ok;
    if (!tried) {
        tried = 1;
        DWORD n = 0;
        HANDLE h = AddFontMemResourceEx((PVOID)trophy_font_data, (DWORD)trophy_font_len, NULL, &n);
        ok = h != NULL && n > 0;
        if (!ok) api->log("trophies: could not load the embedded BB font (using Georgia)");
    }
    return ok ? L"fleshandblood" : L"Georgia";
}

/* The blood-red "BB" in the black circle at (ccx, ccy). The letters are drawn white on a scratch bitmap, their ink
 * box is measured, and they are placed so that the ink (not the font's line box) is centred: this font's B has a long
 * swash below the baseline. A soft dark-red glow goes under them. */
static void draw_bb_mark(unsigned char* px, int cw, int ch, float s, float ccx, float ccy) {
    int sz = (int)(96 * s + 0.5f);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    HDC dc = CreateCompatibleDC(NULL);
    if (!bmp || !dc || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)sz * sz * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(-(int)(24 * s + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, bb_font_name());
    HGDIOBJ oldfont = SelectObject(dc, font);
    RECT rc = {0, 0, sz, sz};
    DrawTextW(dc, L"BB", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    SelectObject(dc, oldfont);
    DeleteObject(font);

    int minx = sz, miny = sz, maxx = -1, maxy = -1;
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++)
            if (bits[((size_t)y * sz + x) * 4 + 1] > 24) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
    if (maxx >= 0) {
        int ox = (int)floorf(ccx - (float)(minx + maxx + 1) / 2.0f + 0.5f);
        int oy = (int)floorf(ccy - (float)(miny + maxy + 1) / 2.0f + 0.5f);
        float glow[3] = {110, 10, 14};
        int gr = (int)(2.0f * s + 0.5f);
        if (gr < 1) gr = 1;
        for (int pass = 0; pass < 2; pass++)
            for (int y = miny - gr; y <= maxy + gr; y++)
                for (int x = minx - gr; x <= maxx + gr; x++) {
                    int X = x + ox, Y = y + oy;
                    if (x < 0 || y < 0 || x >= sz || y >= sz || X < 0 || Y < 0 || X >= cw || Y >= ch) continue;
                    unsigned char* p = px + ((size_t)Y * cw + X) * 4;
                    if (pass == 0) { /* glow: mean coverage around the pixel */
                        float acc = 0;
                        int n = 0;
                        for (int dy = -gr; dy <= gr; dy++)
                            for (int dx = -gr; dx <= gr; dx++) {
                                int xx = x + dx, yy = y + dy;
                                n++;
                                if (xx >= 0 && yy >= 0 && xx < sz && yy < sz) acc += bits[((size_t)yy * sz + xx) * 4 + 1] / 255.0f;
                            }
                        float g = acc / (float)n * 0.9f;
                        if (g > 0.01f) put_mix(p, glow, g > 1 ? 1 : g);
                    } else {
                        float cov = bits[((size_t)y * sz + x) * 4 + 1] / 255.0f;
                        if (cov <= 0) continue;
                        float v = (float)(y - miny) / (float)(maxy - miny + 1); /* brighter at the top */
                        float rgb[3] = {232.0f - 70.0f * v, 54.0f - 26.0f * v, 52.0f - 26.0f * v};
                        put_mix(p, rgb, cov);
                    }
                }
    }
    SelectObject(dc, oldbmp);
    DeleteDC(dc);
    DeleteObject(bmp);
}

/* The two text lines, drawn with GDI over the RGBA8 card. */
static void draw_text(unsigned char* px, int cw, int ch, float s, int tx, const char* line1, const char* name) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    HDC dc = CreateCompatibleDC(NULL);
    if (!bmp || !dc || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    for (size_t i = 0; i < (size_t)cw * ch; i++) {
        bits[i * 4 + 0] = px[i * 4 + 2];
        bits[i * 4 + 1] = px[i * 4 + 1];
        bits[i * 4 + 2] = px[i * 4 + 0];
        bits[i * 4 + 3] = 255;
    }
    SetBkMode(dc, TRANSPARENT);
    int right = cw - (int)(100 * s + 0.5f);
    for (int i = 0; i < 2; i++) {
        int height = (int)((i == 0 ? 25 : 24) * s + 0.5f);
        int top = (int)((i == 0 ? 18 : 62) * s + 0.5f);
        int left = i == 0 ? tx : tx + (int)(36 * s + 0.5f);
        HFONT font = CreateFontW(-height, 0, 0, 0, i == 0 ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Malgun Gothic");
        HGDIOBJ oldfont = SelectObject(dc, font);
        SetTextColor(dc, i == 0 ? RGB(20, 20, 24) : RGB(100, 103, 112));
        wchar_t w[600];
        to_wide(i == 0 ? line1 : name, w, 600);
        RECT rc = {left, top, right, top + height * 2};
        DrawTextW(dc, w, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldfont);
        DeleteObject(font);
    }
    GdiFlush();
    for (size_t i = 0; i < (size_t)cw * ch; i++) {
        px[i * 4 + 0] = bits[i * 4 + 2];
        px[i * 4 + 1] = bits[i * 4 + 1];
        px[i * 4 + 2] = bits[i * 4 + 0];
        px[i * 4 + 3] = 255;
    }
    SelectObject(dc, oldbmp);
    DeleteDC(dc);
    DeleteObject(bmp);
}
#endif

static uint16_t f2h(float f) {
    union { float f; uint32_t u; } v;
    v.f = f;
    uint32_t x = v.u;
    uint32_t sign = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7bffu);
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

/* RGBA8 (sRGB) -> RGBA16F in linear light, times `gain` (the picture is linear, before the post-processing). */
static void to_half(const unsigned char* px, size_t count, float gain, uint16_t* out) {
    static float lut[256];
    static int lut_done;
    if (!lut_done) {
        for (int i = 0; i < 256; i++) {
            float c = (float)i / 255.0f;
            lut[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
        }
        lut_done = 1;
    }
    for (size_t i = 0; i < count; i++) {
        out[i * 4 + 0] = f2h(lut[px[i * 4 + 0]] * gain);
        out[i * 4 + 1] = f2h(lut[px[i * 4 + 1]] * gain);
        out[i * 4 + 2] = f2h(lut[px[i * 4 + 2]] * gain);
        out[i * 4 + 3] = 0x3c00;
    }
}

/* ---------- render side: state shared with the render thread ---------- */

typedef struct { int slot, cw, ch, pos; uint64_t start_ms, dur_ms; } PopState;
static PopState pop_state;
static short pop_x1[2][POP_CAP_H];   /* per slot: row spans of the shape, written before pop_state is published */
static atomic_uint pop_seq;          /* odd while the main thread writes pop_state */
static atomic_int render_state;      /* 0 not seen yet, 1 the copy works, -1 the picture cannot take it */
static atomic_uint pic_w, pic_h;     /* the picture's size, as the hook last saw it */
static unsigned char* stage_ptr;     /* 2 slots of POP_SLOT_BYTES, mapped; set before render_state becomes 1 */
static int pop_next_slot;

typedef struct {
    PFN_vkCmdCopyBufferToImage copy;
    PFN_vkCmdPipelineBarrier2 barrier;
} PopVk;
static PopVk pvk;
static VkBuffer stage_buf;
static int vk_tried;

static int init_vk(const BbRenderDevice* d) {
    if (!d || !d->device || !d->instance || !d->get_device_proc_addr || !d->get_instance_proc_addr) return 0;
    VkDevice dev = (VkDevice)d->device;
    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)d->get_device_proc_addr(d->device, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements get_req =
        (PFN_vkGetBufferMemoryRequirements)d->get_device_proc_addr(d->device, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory)d->get_device_proc_addr(d->device, "vkAllocateMemory");
    PFN_vkBindBufferMemory bind = (PFN_vkBindBufferMemory)d->get_device_proc_addr(d->device, "vkBindBufferMemory");
    PFN_vkMapMemory map = (PFN_vkMapMemory)d->get_device_proc_addr(d->device, "vkMapMemory");
    PFN_vkGetPhysicalDeviceMemoryProperties get_props = (PFN_vkGetPhysicalDeviceMemoryProperties)d->get_instance_proc_addr(
        d->instance, "vkGetPhysicalDeviceMemoryProperties");
    pvk.copy = (PFN_vkCmdCopyBufferToImage)d->get_device_proc_addr(d->device, "vkCmdCopyBufferToImage");
    pvk.barrier = (PFN_vkCmdPipelineBarrier2)d->get_device_proc_addr(d->device, "vkCmdPipelineBarrier2");
    if (!create_buffer || !get_req || !alloc || !bind || !map || !get_props || !pvk.copy || !pvk.barrier) return 0;

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = POP_SLOT_BYTES * 2;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (create_buffer(dev, &bci, NULL, &stage_buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    get_req(dev, stage_buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    get_props((VkPhysicalDevice)d->physical_device, &props);
    uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = -1;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) { type = (int)i; break; }
    if (type < 0) return 0;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    VkDeviceMemory mem;
    if (alloc(dev, &mai, NULL, &mem) != VK_SUCCESS) return 0;
    if (bind(dev, stage_buf, mem, 0) != VK_SUCCESS) return 0;
    void* ptr = NULL;
    if (map(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS || !ptr) return 0;
    stage_ptr = (unsigned char*)ptr;
    return 1;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, uint32_t from, uint32_t to, int entering) {
    VkImageMemoryBarrier2 b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    if (entering) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    b.oldLayout = (VkImageLayout)from;
    b.newLayout = (VkImageLayout)to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di;
    memset(&di, 0, sizeof(di));
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    pvk.barrier(cb, &di);
}

/* The render hook: runs on the render thread; records the copies of the card onto the picture. */
static void on_render(const BbRenderCall* c, void* user) {
    (void)user;
    if (!c || !c->picture.image) return;
    atomic_store(&pic_w, c->picture.width);
    atomic_store(&pic_h, c->picture.height);
    int st = atomic_load(&render_state);
    if (st < 0) return;
    if (st == 0) {
        if (!vk_tried) {
            vk_tried = 1;
            if (!init_vk(c->device)) {
                api->log("trophies: popup off: could not set up the staging buffer");
                atomic_store(&render_state, -1);
                return;
            }
        }
        if (c->picture.format != VK_FORMAT_R16G16B16A16_SFLOAT || !(c->picture.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !(c->picture.flags & BB_IMAGE_WRITE)) {
            api->log("trophies: popup off: the picture is format %u usage 0x%x flags %u (needs RGBA16F, transfer dst, write)",
                     c->picture.format, c->picture.usage, c->picture.flags);
            atomic_store(&render_state, -1);
            return;
        }
        api->log("trophies: popup ready (picture %ux%u)", c->picture.width, c->picture.height);
        atomic_store(&render_state, 1);
    }
    if (!(c->picture.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !(c->picture.flags & BB_IMAGE_WRITE)) return;

    unsigned s1 = atomic_load(&pop_seq);
    if (s1 & 1u) return;
    PopState p = pop_state;
    if (atomic_load(&pop_seq) != s1 || s1 == 0) return;
    if (p.ch > POP_CAP_H || p.slot < 0 || p.slot > 1) return;

    uint64_t t = now_ms() - p.start_ms;
    if (t >= p.dur_ms) return;
    const double slide = 350.0;
    double k = 1.0;
    if ((double)t < slide) k = (double)t / slide;
    else if ((double)t > (double)p.dur_ms - slide) k = ((double)p.dur_ms - (double)t) / slide;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    double e = 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);

    int W = (int)c->picture.width, H = (int)c->picture.height;
    int top = (p.pos & 2) != 0;                        /* bit 1: top (else bottom) */
    int y = top ? H * 11 / 100 : H - H * 6 / 100 - p.ch;
    int x = (int)(-(double)p.cw * (1.0 - e));          /* slides in from the left edge */
    if (x >= W || x + p.cw <= 0 || y >= H || y + p.ch <= 0) return;
    int row0 = y < 0 ? -y : 0;
    int rend = (y + p.ch > H) ? H - y : p.ch;
    if (rend <= row0) return;

    static VkBufferImageCopy regs[POP_CAP_H];
    int nreg = 0;
    const short* sp = pop_x1[p.slot];
    for (int r = row0; r < rend;) {
        int w = sp[r], r2 = r + 1;
        while (r2 < rend && sp[r2] == w) r2++;
        if (w > 0 && nreg < POP_CAP_H) {
            int sx0 = x < 0 ? 0 : x;
            int sx1 = x + w > W ? W : x + w;
            if (sx1 > sx0) {
                VkBufferImageCopy* q = &regs[nreg++];
                memset(q, 0, sizeof(*q));
                q->bufferOffset = (VkDeviceSize)p.slot * POP_SLOT_BYTES + ((VkDeviceSize)r * (VkDeviceSize)p.cw + (VkDeviceSize)(sx0 - x)) * 8u;
                q->bufferRowLength = (uint32_t)p.cw;
                q->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                q->imageSubresource.layerCount = 1;
                q->imageOffset.x = sx0;
                q->imageOffset.y = y + r;
                q->imageExtent.width = (uint32_t)(sx1 - sx0);
                q->imageExtent.height = (uint32_t)(r2 - r);
                q->imageExtent.depth = 1;
            }
        }
        r = r2;
    }
    if (nreg == 0) return;

    VkCommandBuffer cb = (VkCommandBuffer)c->command_buffer;
    VkImage img = (VkImage)c->picture.image;
    image_barrier(cb, img, c->picture.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    pvk.copy(cb, stage_buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)nreg, regs);
    image_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, c->picture.layout, 0);
}

/* ---------- main-thread side ---------- */

#ifdef _WIN32
/* The card for trophy `id` at scale s, RGBA8, *pcw x *pch (malloc'd; NULL when it cannot be made). */
static unsigned char* build_card(int id, int ko, float s, int* pcw, int* pch) {
    int cw = (int)(POP_BASE_W * s + 0.5f), ch = (int)(POP_BASE_H * s + 0.5f);
    if (cw > POP_CAP_W) cw = POP_CAP_W;
    if (ch > POP_CAP_H) ch = POP_CAP_H;
    const TrophyInfo* t = &TROPHIES[id];
    unsigned char* px = (unsigned char*)malloc((size_t)cw * ch * 4);
    if (!px) return NULL;
    unsigned char acc[3];
    grade_color(t->grade, acc);
    int iw = 0, ih = 0;
    unsigned char* icon = load_icon(id, &iw, &ih);
    int tx = compose_base(px, cw, ch, s, acc, icon, iw, ih);
    free(icon);
    draw_text(px, cw, ch, s, tx, ko ? "트로피를 획득했습니다!" : "Trophy earned!", ko ? t->ko_name : t->en_name);
    draw_bb_mark(px, cw, ch, s, (float)cw - 56 * s, (float)ch / 2);
    *pcw = cw;
    *pch = ch;
    return px;
}

/* 2: the popup is at the top of the picture, 0: at the bottom. */
static int popup_pos(void) {
    const char* ps = api->config("trophies.position");
    return (ps && (!strcmp(ps, "bl") || !strcmp(ps, "br"))) ? 0 : 2;
}

/* ---------- the host's overlay image (plugin API 30) ----------
 * The card goes to the host as an RGBA image, and on_frame places it each frame (overlay_tick). The host draws it over
 * the finished frame, after the game's post-processing, so the blur and colour fringing do not touch it, and at the
 * picture's own pixels, so it is sharp. ov_ok is set in bb_plugin_image when the host has the calls. */
static int ov_on, ov_top, ov_cw, ov_ch;
static uint64_t ov_t0, ov_dur;

static int start_overlay_popup(int id, int ko, float seconds) {
    uint32_t aw = 0, ah = 0;
    ov_area_fn(&aw, &ah);
    float s = ah ? (float)ah / 1080.0f : 1.0f; /* made at the picture's own size: one texel to a pixel */
    if (s < 0.5f) s = 0.5f;
    if (s > 2.2f) s = 2.2f;
    int cw = 0, ch = 0;
    unsigned char* px = build_card(id, ko, s, &cw, &ch);
    if (!px) return 0;
    float br = cfg_float("trophies.brightness", 1.0f);
    if (br < 0.2f) br = 0.2f;
    if (br > 1.0f) br = 1.0f;
    if (br < 0.999f)
        for (size_t i = 0; i < (size_t)cw * ch; i++)
            for (int k = 0; k < 3; k++) px[i * 4 + k] = (unsigned char)((float)px[i * 4 + k] * br + 0.5f);
    int rc = ov_set_fn(px, (uint32_t)cw, (uint32_t)ch);
    free(px);
    if (rc != 0) return 0;
    ov_cw = cw;
    ov_ch = ch;
    ov_top = (popup_pos() & 2) != 0;
    ov_t0 = now_ms();
    ov_dur = (uint64_t)(seconds * 1000.0f);
    ov_on = 1;
    return 1;
}

/* Called every frame: slides the card in from the left edge and out again (with a fade), then hides it. */
static void overlay_tick(void) {
    if (!ov_on) return;
    uint64_t t = now_ms() - ov_t0;
    if (t >= ov_dur) {
        ov_place_fn(0, 0, 0, 0, 0);
        ov_on = 0;
        return;
    }
    uint32_t aw = 0, ah = 0;
    ov_area_fn(&aw, &ah);
    if (!aw || !ah) return;
    const double slide = 350.0;
    double k = 1.0;
    if ((double)t < slide) k = (double)t / slide;
    else if ((double)t > (double)ov_dur - slide) k = ((double)ov_dur - (double)t) / slide;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    double e = 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);
    double x = -(double)ov_cw * (1.0 - e);
    double y = ov_top ? (double)(ah * 11u / 100u) : (double)ah - (double)(ah * 6u / 100u) - (double)ov_ch;
    ov_place_fn((float)(x / aw), (float)(y / ah), (float)ov_cw / (float)aw, (float)ov_ch / (float)ah, (float)e);
}
#else
static void overlay_tick(void) {}
#endif

/* Builds the card for trophy `id` and starts showing it. 0 when it cannot (the caller falls back to the banner). */
static int start_popup(int id, int ko, float seconds) {
#ifdef _WIN32
    if (ov_ok) return start_overlay_popup(id, ko, seconds);
    /* Older host: the card is copied onto the picture before the post-processing (blurred by it). */
    if (atomic_load(&render_state) != 1 || !stage_ptr) return 0;
    unsigned H = atomic_load(&pic_h);
    float s = H ? (float)H / 1080.0f : 1.0f;
    if (s < 0.5f) s = 0.5f;
    if (s > 2.2f) s = 2.2f;
    int cw = 0, ch = 0;
    unsigned char* px = build_card(id, ko, s, &cw, &ch);
    if (!px) return 0;

    float gain = cfg_float("trophies.gain", 0.35f);
    if (gain < 0.05f) gain = 0.05f;
    if (gain > 4.0f) gain = 4.0f;
    int slot = pop_next_slot;
    pop_next_slot ^= 1;
    to_half(px, (size_t)cw * ch, gain, (uint16_t*)(stage_ptr + (size_t)slot * POP_SLOT_BYTES));
    free(px);
    compute_spans(pop_x1[slot], cw, ch);

    int pos = popup_pos();

    atomic_fetch_add(&pop_seq, 1u); /* odd: writing */
    pop_state.slot = slot;
    pop_state.cw = cw;
    pop_state.ch = ch;
    pop_state.pos = pos;
    pop_state.start_ms = now_ms();
    pop_state.dur_ms = (uint64_t)(seconds * 1000.0f);
    atomic_fetch_add(&pop_seq, 1u);
    return 1;
#else
    (void)id; (void)ko; (void)seconds;
    return 0;
#endif
}
