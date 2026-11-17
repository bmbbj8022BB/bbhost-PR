/* png_lite.h: a small PNG decoder with its own inflate (no zlib, no stb).
 * 8-bit, non-interlaced: gray, RGB, palette, gray+alpha, RGBA -> RGBA8. Returns malloc'd w*h*4 or NULL. */
#ifndef PNG_LITE_H
#define PNG_LITE_H
#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char* src;
    size_t len, pos;
    unsigned bitbuf;
    int bitcnt;
    unsigned char* out;
    size_t outlen, outcap;
    int err;
} PlInf;
typedef struct { unsigned short count[16]; unsigned short symbol[288]; } PlHuff;

static unsigned pl_bits(PlInf* s, int need) {
    unsigned val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        val |= (unsigned)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1u);
}
static int pl_put(PlInf* s, unsigned char b) {
    if (s->outlen == s->outcap) {
        size_t nc = s->outcap ? s->outcap * 2 : 65536;
        if (nc > ((size_t)1 << 28)) { s->err = 1; return 0; }
        unsigned char* p = (unsigned char*)realloc(s->out, nc);
        if (!p) { s->err = 1; return 0; }
        s->out = p;
        s->outcap = nc;
    }
    s->out[s->outlen++] = b;
    return 1;
}
static int pl_decode(PlInf* s, const PlHuff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)pl_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}
static int pl_construct(PlHuff* h, const short* length, int n) {
    int left = 1;
    for (int i = 0; i <= 15; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    unsigned short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (unsigned short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i] != 0) h->symbol[offs[length[i]]++] = (unsigned short)i;
    return left;
}
static const unsigned short PL_LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned short PL_LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short PL_DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned short PL_DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int pl_codes(PlInf* s, const PlHuff* lc, const PlHuff* dc) {
    for (;;) {
        int sym = pl_decode(s, lc);
        if (sym < 0 || s->err) return -1;
        if (sym < 256) {
            if (!pl_put(s, (unsigned char)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = PL_LBASE[sym] + (int)pl_bits(s, PL_LEXT[sym]);
            int ds = pl_decode(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)PL_DBASE[ds] + pl_bits(s, PL_DEXT[ds]);
            if (s->err || dist > s->outlen) return -1;
            for (; len > 0; len--) {
                unsigned char b = s->out[s->outlen - dist];
                if (!pl_put(s, b)) return -1;
            }
        }
    }
}
static int pl_inflate(PlInf* s) {
    int last;
    do {
        last = (int)pl_bits(s, 1);
        int type = (int)pl_bits(s, 2);
        if (s->err) return -1;
        if (type == 0) {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->pos + 4 > s->len) return -1;
            unsigned len = s->src[s->pos] | (s->src[s->pos + 1] << 8);
            unsigned nlen = s->src[s->pos + 2] | (s->src[s->pos + 3] << 8);
            s->pos += 4;
            if (len != (~nlen & 0xffffu) || s->pos + len > s->len) return -1;
            for (unsigned i = 0; i < len; i++)
                if (!pl_put(s, s->src[s->pos++])) return -1;
        } else if (type == 1) {
            PlHuff lc, dc;
            short lengths[320];
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            pl_construct(&lc, lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            pl_construct(&dc, lengths, 30);
            if (pl_codes(s, &lc, &dc)) return -1;
        } else if (type == 2) {
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320];
            PlHuff lc, dc, cc;
            int nlen = (int)pl_bits(s, 5) + 257;
            int ndist = (int)pl_bits(s, 5) + 1;
            int ncode = (int)pl_bits(s, 4) + 4;
            if (s->err || nlen > 286 || ndist > 30) return -1;
            int index;
            for (index = 0; index < ncode; index++) lengths[order[index]] = (short)pl_bits(s, 3);
            for (; index < 19; index++) lengths[order[index]] = 0;
            if (pl_construct(&cc, lengths, 19) != 0) return -1;
            index = 0;
            while (index < nlen + ndist) {
                int sym = pl_decode(s, &cc);
                if (sym < 0 || s->err) return -1;
                if (sym < 16) {
                    lengths[index++] = (short)sym;
                } else {
                    int rep, prev = 0;
                    if (sym == 16) {
                        if (index == 0) return -1;
                        prev = lengths[index - 1];
                        rep = 3 + (int)pl_bits(s, 2);
                    } else if (sym == 17) {
                        rep = 3 + (int)pl_bits(s, 3);
                    } else {
                        rep = 11 + (int)pl_bits(s, 7);
                    }
                    if (index + rep > nlen + ndist) return -1;
                    while (rep--) lengths[index++] = (short)prev;
                }
            }
            if (lengths[256] == 0) return -1;
            if (pl_construct(&lc, lengths, nlen) < 0) return -1;
            if (pl_construct(&dc, lengths + nlen, ndist) < 0) return -1;
            if (pl_codes(s, &lc, &dc)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

static unsigned pl_be32(const unsigned char* p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static unsigned char* png_decode_rgba(const unsigned char* d, size_t n, int* ow, int* oh) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n < 33 || memcmp(d, sig, 8)) return NULL;
    size_t pos = 8;
    unsigned w = 0, h = 0;
    int bd = 0, ct = -1, il = 0;
    unsigned char pal[256 * 4];
    int npal = 0;
    unsigned char* idat = NULL;
    size_t idl = 0, idc = 0;
    unsigned char* res = NULL;
    for (int i = 0; i < 256; i++) { pal[i * 4] = pal[i * 4 + 1] = pal[i * 4 + 2] = 0; pal[i * 4 + 3] = 255; }
    while (pos + 12 <= n) {
        unsigned len = pl_be32(d + pos);
        const unsigned char* type = d + pos + 4;
        const unsigned char* body = d + pos + 8;
        if ((size_t)len > n - pos - 12) goto done;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = pl_be32(body);
            h = pl_be32(body + 4);
            bd = body[8];
            ct = body[9];
            il = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(len / 3);
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) memcpy(pal + i * 4, body + i * 3, 3);
        } else if (!memcmp(type, "tRNS", 4) && ct == 3) {
            for (unsigned i = 0; i < len && i < 256; i++) pal[i * 4 + 3] = body[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idl + len > idc) {
                idc = (idl + len) * 2;
                unsigned char* p = (unsigned char*)realloc(idat, idc);
                if (!p) goto done;
                idat = p;
            }
            memcpy(idat + idl, body, len);
            idl += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    {
        if (bd != 8 || il != 0 || w == 0 || h == 0 || w > 4096 || h > 4096 || !idat || idl < 6) goto done;
        int ch;
        switch (ct) { case 0: ch = 1; break; case 2: ch = 3; break; case 3: ch = 1; break; case 4: ch = 2; break; case 6: ch = 4; break; default: goto done; }
        PlInf s;
        memset(&s, 0, sizeof(s));
        s.src = idat + 2; /* zlib header */
        s.len = idl - 2;
        if (pl_inflate(&s) != 0) { free(s.out); goto done; }
        size_t stride = (size_t)w * ch;
        if (s.outlen < (stride + 1) * h) { free(s.out); goto done; }
        unsigned char* raw = s.out;
        for (unsigned y = 0; y < h; y++) {
            unsigned char* row = raw + y * (stride + 1);
            int ft = row[0];
            unsigned char* cur = row + 1;
            const unsigned char* up = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)ch ? cur[x - ch] : 0;
                int b = up ? up[x] : 0;
                int c = (up && x >= (size_t)ch) ? up[x - ch] : 0;
                int v = cur[x];
                switch (ft) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: {
                    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: free(raw); goto done;
                }
                cur[x] = (unsigned char)v;
            }
        }
        res = (unsigned char*)malloc((size_t)w * h * 4);
        if (res) {
            for (unsigned y = 0; y < h; y++) {
                const unsigned char* src = raw + y * (stride + 1) + 1;
                unsigned char* dst = res + (size_t)y * w * 4;
                for (unsigned x = 0; x < w; x++, dst += 4) {
                    switch (ct) {
                    case 0: dst[0] = dst[1] = dst[2] = src[x]; dst[3] = 255; break;
                    case 2: dst[0] = src[x * 3]; dst[1] = src[x * 3 + 1]; dst[2] = src[x * 3 + 2]; dst[3] = 255; break;
                    case 3: memcpy(dst, pal + src[x] * 4, 4); break;
                    case 4: dst[0] = dst[1] = dst[2] = src[x * 2]; dst[3] = src[x * 2 + 1]; break;
                    default: memcpy(dst, src + x * 4, 4); break;
                    }
                }
            }
            *ow = (int)w;
            *oh = (int)h;
        }
        free(raw);
    }
done:
    free(idat);
    return res;
}
#endif
