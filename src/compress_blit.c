#define _GNU_SOURCE
#include "demo.h"
#include "compress_blit.h"
#include <arm_neon.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Accelerate DrawOP_COPY/ADD_Compress_16 (±clip) for palette (+0xFFFB) streams.
 * Unclipped COPY uses a span cache keyed by (src,pal,skip) so repeated map tiles
 * become memcpy of pre-expanded RGB565 runs.
 */

enum { CACHE_N = 256, SPAN_MAX = 8192, COLOR_MAX = 800 * 600 };

typedef struct { uint32_t offset; uint16_t count; } Span;
typedef struct {
    const unsigned char *src;
    const uint16_t *pal;
    int skip, span_n, color_n;
    size_t len;
    unsigned char *snap;
    Span *spans;
    uint16_t *colors;
    unsigned age;
} Cache;

static Cache cache[CACHE_N];
static unsigned cache_age, hits, misses, fallbacks;
static Span tmp_spans[SPAN_MAX];
static uint16_t tmp_colors[COLOR_MAX];

static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

static int supported_src(const unsigned char *src) {
    unsigned f = le16(src);
    return f < 0xfff9 || f == 0xfffb;
}

static Cache *decode_copy(const unsigned char *src, const uint16_t *pal, int skip) {
    unsigned first = le16(src);
    if (first == 0xfff9 || first == 0xfffa || (first >= 0xfffc && first <= 0xfffd)) return NULL;
    for (int i = 0; i < CACHE_N; i++) {
        Cache *c = cache + i;
        if (c->spans && c->src == src && c->pal == pal && c->skip == skip && c->snap &&
            !memcmp(src, c->snap, c->len)) {
            c->age = ++cache_age;
            hits++;
            return c;
        }
    }
    misses++;
    const unsigned char *p = src + (first == 0xfffb ? 10 : 0);
    int offset = 0, span_n = 0, color_n = 0;
    for (;;) {
        unsigned cmd = le16(p);
        p += 2;
        if (cmd == 0xffff) break;
        if (cmd == 0xfffe) {
            offset += skip;
            continue;
        }
        if (!(cmd & 0x8000)) {
            offset += (int)cmd;
            continue;
        }
        unsigned count = cmd & 0x7fff;
        if (span_n >= SPAN_MAX || color_n + (int)count > COLOR_MAX) return NULL;
        tmp_spans[span_n].offset = (uint32_t)offset;
        tmp_spans[span_n].count = (uint16_t)count;
        for (unsigned j = 0; j < count; j++) tmp_colors[color_n + (int)j] = pal[p[j]];
        span_n++;
        color_n += (int)count;
        offset += (int)count;
        p += count;
        if ((size_t)(p - src) > 4u * 1024u * 1024u) return NULL;
    }
    size_t len = (size_t)(p - src);
    int slot = 0;
    for (int i = 0; i < CACHE_N; i++) {
        if (!cache[i].spans) {
            slot = i;
            break;
        }
        if (cache[i].age < cache[slot].age) slot = i;
    }
    Cache *c = cache + slot;
    free(c->snap);
    free(c->spans);
    free(c->colors);
    memset(c, 0, sizeof(*c));
    c->snap = malloc(len);
    c->spans = malloc((size_t)span_n * sizeof(Span) + 8);
    c->colors = malloc((size_t)color_n * 2 + 8);
    if (!c->snap || !c->spans || !c->colors) {
        free(c->snap);
        free(c->spans);
        free(c->colors);
        memset(c, 0, sizeof(*c));
        return NULL;
    }
    memcpy(c->snap, src, len);
    memcpy(c->spans, tmp_spans, (size_t)span_n * sizeof(Span));
    memcpy(c->colors, tmp_colors, (size_t)color_n * 2);
    c->src = src;
    c->pal = pal;
    c->skip = skip;
    c->span_n = span_n;
    c->color_n = color_n;
    c->len = len;
    c->age = ++cache_age;
    return c;
}

static void blit_cached(uint16_t *dst, const Cache *c) {
    const uint16_t *cols = c->colors;
    for (int i = 0; i < c->span_n; i++) {
        memcpy(dst + c->spans[i].offset, cols, (size_t)c->spans[i].count * 2);
        cols += c->spans[i].count;
    }
}

static void copy_run(uint16_t *dst, const unsigned char *idx, const uint16_t *pal, unsigned count) {
    unsigned i = 0;
    for (; i + 8 <= count; i += 8) {
        uint8x8_t ix = vld1_u8(idx + i);
        uint16_t tmp[8] = {
            pal[vget_lane_u8(ix, 0)], pal[vget_lane_u8(ix, 1)], pal[vget_lane_u8(ix, 2)], pal[vget_lane_u8(ix, 3)],
            pal[vget_lane_u8(ix, 4)], pal[vget_lane_u8(ix, 5)], pal[vget_lane_u8(ix, 6)], pal[vget_lane_u8(ix, 7)]};
        vst1q_u16(dst + i, vld1q_u16(tmp));
    }
    for (; i < count; i++) dst[i] = pal[idx[i]];
}

static void add_pixel(uint16_t *d, uint16_t s, int amount) {
    int sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
    int dr = *d >> 11, dg = (*d >> 5) & 63, db = *d & 31;
    if (amount == 255) {
        int r = dr + sr;
        if (r > 31) r = 31;
        int g = dg + sg;
        if (g > 63) g = 63;
        int b = db + sb;
        if (b > 31) b = 31;
        *d = (uint16_t)((r << 11) | (g << 5) | b);
        return;
    }
    int r = dr + ((sr * amount) >> 8);
    if (r > 31) r = 31;
    int g = dg + ((sg * amount) >> 8);
    if (g > 63) g = 63;
    int b = db + ((sb * amount) >> 8);
    if (b > 31) b = 31;
    *d = (uint16_t)((r << 11) | (g << 5) | b);
}

static void add_run(uint16_t *dst, const unsigned char *idx, const uint16_t *pal, unsigned count, int amount) {
    for (unsigned i = 0; i < count; i++) add_pixel(dst + i, pal[idx[i]], amount);
}

static void stream_copy(uint16_t *dst, const unsigned char *src, const uint16_t *pal, int skip, int left, int right,
                        int top, int bottom, int clipped) {
    unsigned first = le16(src);
    const unsigned char *p = src + (first == 0xfffb ? 10 : 0);
    int byte_skip = skip * 2, x = 0, y = 0;
    for (;;) {
        unsigned cmd = le16(p);
        p += 2;
        if (cmd == 0xffff) return;
        if (cmd == 0xfffe) {
            dst = (uint16_t *)((unsigned char *)dst + byte_skip);
            x = 0;
            y++;
            if (clipped && y >= bottom) return;
            continue;
        }
        if (!(cmd & 0x8000)) {
            dst += cmd;
            x += (int)cmd;
            continue;
        }
        unsigned count = cmd & 0x7fff;
        if (!clipped || (y >= top && y < bottom && x < right && x + (int)count > left)) {
            unsigned start = 0, n = count;
            uint16_t *out = dst;
            const unsigned char *ix = p;
            if (clipped) {
                if (x < left) {
                    start = (unsigned)(left - x);
                    out += start;
                    ix += start;
                    n -= start;
                }
                if (x + (int)start + (int)n > right) n = (unsigned)(right - (x + (int)start));
            }
            if (n) copy_run(out, ix, pal, n);
        }
        dst += count;
        p += count;
        x += (int)count;
    }
}

static void stream_add(uint16_t *dst, const unsigned char *src, const uint16_t *pal, int skip, int left, int right,
                       int top, int bottom, int clipped, int amount) {
    unsigned first = le16(src);
    const unsigned char *p = src + (first == 0xfffb ? 10 : 0);
    int byte_skip = skip * 2, x = 0, y = 0;
    for (;;) {
        unsigned cmd = le16(p);
        p += 2;
        if (cmd == 0xffff) return;
        if (cmd == 0xfffe) {
            dst = (uint16_t *)((unsigned char *)dst + byte_skip);
            x = 0;
            y++;
            if (clipped && y >= bottom) return;
            continue;
        }
        if (!(cmd & 0x8000)) {
            dst += cmd;
            x += (int)cmd;
            continue;
        }
        unsigned count = cmd & 0x7fff;
        if (!clipped || (y >= top && y < bottom && x < right && x + (int)count > left)) {
            unsigned start = 0, n = count;
            uint16_t *out = dst;
            const unsigned char *ix = p;
            if (clipped) {
                if (x < left) {
                    start = (unsigned)(left - x);
                    out += start;
                    ix += start;
                    n -= start;
                }
                if (x + (int)start + (int)n > right) n = (unsigned)(right - (x + (int)start));
            }
            if (n) add_run(out, ix, pal, n, amount);
        }
        dst += count;
        p += count;
        x += (int)count;
    }
}

static void (*orig_copy)(uint16_t *, const unsigned char *, const uint16_t *, int);
static void (*orig_copy_clip)(uint16_t *, const unsigned char *, const uint16_t *, int, int, int, int, int);
static void (*orig_add)(uint16_t *, const unsigned char *, const uint16_t *, int, long);
static void (*orig_add_clip)(uint16_t *, const unsigned char *, const uint16_t *, int, int, int, int, int, long);

static void hook_copy(uint16_t *d, const unsigned char *s, const uint16_t *p, int n) {
    if (!supported_src(s)) {
        fallbacks++;
        orig_copy(d, s, p, n);
        return;
    }
    Cache *c = decode_copy(s, p, n);
    if (c) {
        blit_cached(d, c);
        return;
    }
    stream_copy(d, s, p, n, 0, 0, 0, 0, 0);
}

static void hook_copy_clip(uint16_t *d, const unsigned char *s, const uint16_t *p, int n, int l, int w, int t, int h) {
    if (!supported_src(s)) {
        fallbacks++;
        orig_copy_clip(d, s, p, n, l, w, t, h);
        return;
    }
    stream_copy(d, s, p, n, l, l + w, t, t + h, 1);
}

static void hook_add(uint16_t *d, const unsigned char *s, const uint16_t *p, int n, long a) {
    if (!supported_src(s) || a <= 0 || a > 255) {
        fallbacks++;
        orig_add(d, s, p, n, a);
        return;
    }
    stream_add(d, s, p, n, 0, 0, 0, 0, 0, (int)a);
}

static void hook_add_clip(uint16_t *d, const unsigned char *s, const uint16_t *p, int n, int l, int w, int t, int h,
                          long a) {
    if (!supported_src(s) || a <= 0 || a > 255) {
        fallbacks++;
        orig_add_clip(d, s, p, n, l, w, t, h, a);
        return;
    }
    stream_add(d, s, p, n, l, l + w, t, t + h, 1, (int)a);
}

void compress_blit_init(void) {
    orig_copy = demo_hook(0x1142e0, hook_copy);
    orig_copy_clip = demo_hook(0x1144c8, hook_copy_clip);
    orig_add = demo_hook(0x101fac, hook_add);
    orig_add_clip = demo_hook(0x102218, hook_add_clip);
    game_log("CPUOPT COPY/ADD compress hooks ready (cache=%d)\n", CACHE_N);
}

/* ---- pixel check (no hooks) ---- */
static unsigned char image[65536];
static uint16_t buf_a[800 * 600], buf_b[800 * 600], pal[256];
static unsigned rng = 1;
static unsigned random_u(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}
static void put16(unsigned char **p, unsigned v) {
    *(*p)++ = (unsigned char)v;
    *(*p)++ = (unsigned char)(v >> 8);
}
static void make_image(int alpha_header) {
    memset(image, 0, sizeof(image));
    unsigned char *p = image + (alpha_header ? 10 : 0);
    for (int y = 0; y < 24; y++) {
        put16(&p, 2 + (y & 3));
        unsigned count = 20 + (y & 7);
        put16(&p, 0x8000 | count);
        for (unsigned j = 0; j < count; j++) *p++ = (unsigned char)random_u();
        if (y != 23) put16(&p, 0xfffe);
    }
    put16(&p, 0xffff);
    if (alpha_header) {
        image[0] = 0xfb;
        image[1] = 0xff;
        unsigned off = (unsigned)(p - image - 2);
        for (int i = 0; i < 4; i++) image[2 + i] = (unsigned char)(off >> (8 * i));
    }
}

int compress_blit_pixel_check(uintptr_t base) {
    void (*native_copy)(uint16_t *, const unsigned char *, const uint16_t *, int) = (void *)(base + 0x1142e0);
    void (*native_copy_clip)(uint16_t *, const unsigned char *, const uint16_t *, int, int, int, int, int) =
        (void *)(base + 0x1144c8);
    void (*native_add)(uint16_t *, const unsigned char *, const uint16_t *, int, long) = (void *)(base + 0x101fac);
    void (*native_add_clip)(uint16_t *, const unsigned char *, const uint16_t *, int, int, int, int, int, long) =
        (void *)(base + 0x102218);
    int tests = 0;
    for (int alpha = 0; alpha < 2; alpha++) {
        for (int t = 0; t < 8; t++) {
            make_image(alpha);
            for (int i = 0; i < 256; i++) pal[i] = (uint16_t)random_u();
            for (int i = 0; i < 800 * 600; i++) buf_a[i] = buf_b[i] = (uint16_t)random_u();
            int start = 40 * 800 + 60;
            int skip = 800 - 32;
            native_copy(buf_a + start, image, pal, skip);
            stream_copy(buf_b + start, image, pal, skip, 0, 0, 0, 0, 0);
            if (memcmp(buf_a, buf_b, sizeof(buf_a))) {
                game_log("FAIL copy alpha=%d t=%d\n", alpha, t);
                return 1;
            }
            tests++;

            for (int i = 0; i < 800 * 600; i++) buf_a[i] = buf_b[i] = (uint16_t)random_u();
            native_copy_clip(buf_a + start, image, pal, skip, 4, 18, 2, 12);
            stream_copy(buf_b + start, image, pal, skip, 4, 22, 2, 14, 1);
            if (memcmp(buf_a, buf_b, sizeof(buf_a))) {
                game_log("FAIL copy_clip alpha=%d t=%d\n", alpha, t);
                return 2;
            }
            tests++;

            int amounts[] = {1, 31, 127, 200, 254, 255};
            for (int ai = 0; ai < 6; ai++) {
                for (int i = 0; i < 800 * 600; i++) buf_a[i] = buf_b[i] = (uint16_t)random_u();
                native_add(buf_a + start, image, pal, skip, amounts[ai]);
                stream_add(buf_b + start, image, pal, skip, 0, 0, 0, 0, 0, amounts[ai]);
                if (memcmp(buf_a, buf_b, sizeof(buf_a))) {
                    game_log("FAIL add alpha=%d amount=%d t=%d\n", alpha, amounts[ai], t);
                    return 3;
                }
                tests++;
                for (int i = 0; i < 800 * 600; i++) buf_a[i] = buf_b[i] = (uint16_t)random_u();
                native_add_clip(buf_a + start, image, pal, skip, 3, 20, 1, 14, amounts[ai]);
                stream_add(buf_b + start, image, pal, skip, 3, 23, 1, 15, 1, amounts[ai]);
                if (memcmp(buf_a, buf_b, sizeof(buf_a))) {
                    game_log("FAIL add_clip alpha=%d amount=%d t=%d\n", alpha, amounts[ai], t);
                    return 4;
                }
                tests++;
            }

            /* cache path vs stream */
            for (int i = 0; i < 800 * 600; i++) buf_a[i] = buf_b[i] = (uint16_t)random_u();
            Cache *c = decode_copy(image, pal, skip);
            if (!c) {
                game_log("FAIL cache decode alpha=%d t=%d\n", alpha, t);
                return 5;
            }
            blit_cached(buf_a + start, c);
            stream_copy(buf_b + start, image, pal, skip, 0, 0, 0, 0, 0);
            if (memcmp(buf_a, buf_b, sizeof(buf_a))) {
                game_log("FAIL cache blit alpha=%d t=%d\n", alpha, t);
                return 6;
            }
            tests++;
        }
    }
    game_log("PASS compress_blit pixel check tests=%d hits=%u misses=%u\n", tests, hits, misses);
    return 0;
}
