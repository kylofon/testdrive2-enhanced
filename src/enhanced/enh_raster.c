/* Enhanced renderer: sprite decoding, the indexed sample buffers and their rasterisation (ENHANCED.md
 * "Pixels").
 *
 * A sample buffer holds palette indices at enh_sq samples per original pixel. Every display-list entry is
 * applied in order with the original operation: fills and spans set a colour, sprites combine the colour
 * with their stored planes through the blitters' AND / OR / XOR / replace rules. Rendering runs in
 * horizontal bands on the host worker pool; each band also resolves its output rows through the current
 * palette (average of the samples in linear light). There are two targets, the front view and the mirror,
 * each with its own scene (display list) and buffers. */
#include "enh_internal.h"
#include "../host.h"
#include "../platform/gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int enh_scale, enh_q, enh_sq;
EnhTarget enh_front = { .sc = &enh_sc, .vw = VIEW_W, .vh = VIEW_H };
EnhTarget enh_mirror = { .sc = &enh_mc, .vw = MIRROR_W, .vh = MIRROR_H };

/* ------------------------------------------------------------------------------------------------ */
/* sprites                                                                                          */

#define SPR_CACHE 1024
static EnhSprite cache[SPR_CACHE];
static int ncache;

void enh_sprite_cache_clear(void)
{
    for (int i = 0; i < ncache; i++) {
        free(cache[i].bits);
        for (int op = 0; op < 4; op++) free(cache[i].mip[op]);
    }
    ncache = 0;
}

static void make_luts(EnhSprite *s, const u8 pm[4])
{
    /* destination list as the RAM blitter builds it: (plane, stored block), at most 4 entries */
    int eplane[4], eblock[4], n = 0;
    for (int p = 0; p < 4 && n < 4; p++) {
        u8 bx = pm[p] & 0x0F;
        if (!bx) break;
        for (int b = 0; b < 4 && n < 4; b++)
            if (bx & (1 << b)) { eplane[n] = b; eblock[n] = p; n++; }
    }
    u8 clr = (u8)(pm[0] >> 4), set = (u8)(pm[1] >> 4);
    for (int op = 0; op < 4; op++) {
        s->touch[op] = 0;
        for (int v = 0; v < 16; v++) {
            for (int c = 0; c < 16; c++) {
                u8 x = (u8)c;
                switch (op) {
                case EOP_COPY:
                    x = (u8)(x & ~clr);
                    x |= set;
                    for (int e = n - 1; e >= 0; e--) {      /* processed last to first */
                        u8 m = (u8)(1 << eplane[e]);
                        x = (v >> eblock[e]) & 1 ? (u8)(x | m) : (u8)(x & ~m);
                    }
                    break;
                case EOP_AND:
                    x = (u8)(x & ~clr);
                    for (int e = 0; e < n; e++)
                        if (!((v >> eblock[e]) & 1)) x = (u8)(x & ~(1 << eplane[e]));
                    break;
                case EOP_OR:
                    x |= set;
                    for (int e = 0; e < n; e++)
                        if ((v >> eblock[e]) & 1) x = (u8)(x | (1 << eplane[e]));
                    break;
                default:
                    x ^= set;
                    for (int e = 0; e < n; e++)
                        if ((v >> eblock[e]) & 1) x = (u8)(x ^ (1 << eplane[e]));
                    break;
                }
                x &= 15;
                s->lut[op][v << 4 | c] = x;
                if (x != c) s->touch[op] |= (u16)(1 << v);
            }
        }
    }
}

/* Reduced copies of a sprite for each operation (the sample buffer holds palette indices, so texels cannot
 * be averaged): a texel of level L covers 2^L x 2^L source pixels and holds the most frequent of their
 * patterns that change something under the operation, and the share of such pixels. Drawn with that share
 * as a dithered coverage, a sprite scaled far down keeps its average shape and colour from frame to frame
 * instead of sparkling between the source pixels a sample happens to hit. */
static void build_mips(EnhSprite *s)
{
    int n = 0, off = 0;
    for (int L = 1; L <= ENH_MIPS; L++) {
        int w = (s->w + (1 << L) - 1) >> L, h = (s->h + (1 << L) - 1) >> L;
        s->mip_w[L] = w;
        s->mip_h[L] = h;
        s->mip_off[L] = off;
        off += w * h;
        n = L;
        if (w <= 1 && h <= 1) break;
    }
    s->nmip = n;
    for (int op = 0; op < 4; op++) {
        u8 *m = malloc((size_t)off * 2);
        s->mip[op] = m;
        if (!m) { s->nmip = 0; continue; }
        u16 touch = s->touch[op];
        for (int L = 1; L <= n; L++) {
            int bs = 1 << L;
            for (int ty = 0; ty < s->mip_h[L]; ty++)
                for (int tx = 0; tx < s->mip_w[L]; tx++) {
                    int cnt[16] = { 0 }, area = 0, hit = 0;
                    for (int y = ty * bs; y < ty * bs + bs && y < s->h; y++)
                        for (int x = tx * bs; x < tx * bs + bs && x < s->w; x++) {
                            u8 v = s->bits[y * s->w + x];
                            area++;
                            if (touch & (1 << v)) { cnt[v]++; hit++; }
                        }
                    int best = 0;
                    for (int v = 1; v < 16; v++) if (cnt[v] > cnt[best]) best = v;
                    u8 *t = m + 2 * (s->mip_off[L] + ty * s->mip_w[L] + tx);
                    t[0] = (u8)(hit ? best : 0);
                    t[1] = (u8)(area ? (hit * 255 + area / 2) / area : 0);
                }
        }
    }
}

const EnhSprite *enh_sprite(FarPtr p)
{
    if (p.seg == 0) return NULL;
    for (int i = 0; i < ncache; i++)
        if (cache[i].seg == p.seg && cache[i].off == p.off) return &cache[i];
    if (ncache == SPR_CACHE) return NULL;
    u16 wb = rd16(p.seg, p.off), h = rd16(p.seg, (u16)(p.off + 2));
    if (wb == 0 || h == 0 || wb > 255 || h > 255) return NULL;
    EnhSprite *s = &cache[ncache];
    memset(s, 0, sizeof *s);
    s->seg = p.seg;
    s->off = p.off;
    s->w = wb * 8;
    s->h = h;
    s->hx = (s16)rd16(p.seg, (u16)(p.off + 4));
    s->hy = (s16)rd16(p.seg, (u16)(p.off + 6));
    s->ox = (s16)rd16(p.seg, (u16)(p.off + 8));
    s->oy = (s16)rd16(p.seg, (u16)(p.off + 10));
    u8 pm[4];
    for (int k = 0; k < 4; k++) pm[k] = rd8(p.seg, (u16)(p.off + 12 + k));
    int nst = 0;
    while (nst < 4 && (pm[nst] & 0x0F)) nst++;
    u16 block = (u16)((u8)h * (u8)wb);
    if (pm[3] & 0xF0) block = (u16)(block + (pm[3] >> 4));
    s->bits = calloc((size_t)s->w * h, 1);
    if (!s->bits) return NULL;
    for (int k = 0; k < nst; k++) {
        u16 src = (u16)(p.off + 0x10 + k * block);
        for (int y = 0; y < h; y++) {
            u16 row = (u16)(src + y * wb);
            for (int x = 0; x < s->w; x++)
                if (rd8(p.seg, (u16)(row + (x >> 3))) & (0x80 >> (x & 7)))
                    s->bits[y * s->w + x] |= (u8)(1 << k);
        }
    }
    make_luts(s, pm);
    build_mips(s);
    ncache++;
    return s;
}

void enh_cover_sprite(u8 *cover, int cw, int chh, const EnhSprite *s, int x, int y, int op)
{
    if (!s) return;
    for (int j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= chh) continue;
        for (int i = 0; i < s->w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= cw) continue;
            if (s->touch[op] & (1 << s->bits[j * s->w + i])) cover[yy * cw + xx] = 1;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* buffers                                                                                          */

static void target_free(EnhTarget *t)
{
    free(t->smp); free(t->g_near); free(t->g_far); free(t->g_t); free(t->g_l); free(t->g_r); free(t->tx_buf);
    free(t->out);
    t->smp = NULL; t->g_near = t->g_far = NULL; t->g_t = t->g_l = t->g_r = NULL; t->tx_buf = NULL; t->out = NULL;
}

static bool target_alloc(EnhTarget *t, int k)
{
    target_free(t);
    t->sw = t->vw * enh_sq;
    t->sh = t->vh * enh_sq;
    t->ow = t->vw * k;
    t->oh = t->vh * k;
    t->smp = malloc((size_t)t->sw * t->sh);
    t->g_near = malloc((size_t)t->sh * sizeof *t->g_near);
    t->g_far = malloc((size_t)t->sh * sizeof *t->g_far);
    t->g_t = malloc((size_t)t->sh * sizeof *t->g_t);
    t->g_l = malloc((size_t)t->sh * sizeof *t->g_l);
    t->g_r = malloc((size_t)t->sh * sizeof *t->g_r);
    t->tx_buf = malloc((size_t)ENH_MAX_BANDS * (size_t)t->sw * sizeof *t->tx_buf);
    t->out = calloc((size_t)t->ow * t->oh, sizeof *t->out);
    if (!t->smp || !t->g_near || !t->g_far || !t->g_t || !t->g_l || !t->g_r || !t->tx_buf || !t->out) return false;
    t->nbands = t->oh / 4 < ENH_MAX_BANDS ? t->oh / 4 : ENH_MAX_BANDS;
    if (t->nbands < 1) t->nbands = 1;
    for (int i = 0; i <= t->nbands; i++) t->band_o0[i] = t->oh * i / t->nbands;
    t->pal_key = 0;
    return true;
}

bool enh_raster_setup(void)
{
    int k = gfx_output_scale();
    if (k == enh_scale && enh_front.smp && enh_mirror.smp) return true;
    enh_scale = k;
    enh_q = k == 1 ? 4 : 2;
    enh_sq = k * enh_q;
    if (!target_alloc(&enh_front, k) || !target_alloc(&enh_mirror, k)) {
        target_free(&enh_front);
        target_free(&enh_mirror);
        enh_scale = 0;
        return false;
    }
    return true;
}

/* sample index of the first sample whose centre is at or after v (original coordinates) */
static inline int sidx(float v) { return (int)ceil(v * (float)enh_sq - 0.5f); }
static inline float scen(int i) { return ((float)i + 0.5f) / (float)enh_sq; }

static const u8 BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
static inline bool dither_pass(float alpha, int c, int r)
{
    return (float)BAYER[r & 3][c & 3] + 0.5f < alpha * 16.0f;
}

typedef struct {
    const EnhTarget *t;
    const EnhScene *S;
    int r0, r1, band;
    float yoff;                   /* scene y of output y 0 (falling) */
} Band;

static void row_range(const Band *b, float y0, float y1, int *ra, int *rb)
{
    int a = sidx(y0 - b->yoff), e = sidx(y1 - b->yoff);
    if (a < b->r0) a = b->r0;
    if (e > b->r1) e = b->r1;
    *ra = a;
    *rb = e;
}

static void col_range(const Band *b, float x0, float x1, int *ca, int *cb)
{
    if (x0 < 0) x0 = 0;
    if (x1 > b->t->vw) x1 = (float)b->t->vw;
    int a = sidx(x0), e = sidx(x1);
    if (a < 0) a = 0;
    if (e > b->t->sw) e = b->t->sw;
    *ca = a;
    *cb = e;
}

static void span(const Band *b, int r, float x0, float x1, u8 colour, float alpha)
{
    int ca, cb;
    col_range(b, x0, x1, &ca, &cb);
    if (ca >= cb) return;
    u8 *row = b->t->smp + (size_t)r * b->t->sw;
    if (alpha >= 1) {
        memset(row + ca, colour, (size_t)(cb - ca));
        return;
    }
    for (int c = ca; c < cb; c++)
        if (dither_pass(alpha, c, r)) row[c] = colour;
}

/* ------------------------------------------------------------------------------------------------ */
/* commands                                                                                         */

static float clip_lo(const EnhCmd *c, float y) { float v = c->cy0 > y ? c->cy0 : y; return v > 0 ? v : 0; }
static float clip_hi(const Band *b, const EnhCmd *c, float y)
{
    float v = c->cy1 < y ? c->cy1 : y;
    return v < b->t->vh ? v : (float)b->t->vh;
}

static float cx_lo(const EnhCmd *c, float x) { return c->cx0 > x ? c->cx0 : x; }
static float cx_hi(const EnhCmd *c, float x) { return c->cx1 < x ? c->cx1 : x; }

static void do_fill(const Band *b, const EnhCmd *c)
{
    int ra, rb;
    row_range(b, clip_lo(c, c->y0), clip_hi(b, c, c->y1), &ra, &rb);
    for (int r = ra; r < rb; r++) span(b, r, cx_lo(c, c->x0), cx_hi(c, c->x1), c->colour, c->alpha);
}

static void do_sprite(const Band *b, const EnhCmd *c)
{
    const EnhTarget *T = b->t;
    const EnhSprite *s = c->spr;
    float k = c->x1;
    float xe = c->x0 + (float)s->w * k, ye = c->y0 + (float)s->h * k;
    int ra, rb, ca, cb;
    row_range(b, clip_lo(c, c->y0), clip_hi(b, c, ye), &ra, &rb);
    if (ra >= rb) return;
    col_range(b, cx_lo(c, c->x0), cx_hi(c, xe), &ca, &cb);
    if (ca >= cb) return;
    const u8 *lut = s->lut[c->op];
    u16 touch = s->touch[c->op];
    int *tx = T->tx_buf + (size_t)b->band * T->sw;
    float inv = 1.0f / k;
    for (int col = ca; col < cb; col++) {
        int t = (int)floor((scen(col) - c->x0) * inv);
        tx[col] = t < 0 ? 0 : t >= s->w ? s->w - 1 : t;
    }
    /* source pixels per output pixel: from two on, the reduced copy whose texels are about that size */
    int L = 0;
    for (float px = inv / (float)enh_scale; L < s->nmip && px >= (float)(2 << L); ) L++;
    if (L > 0 && s->mip[c->op]) {
        const u8 *m = s->mip[c->op] + 2 * s->mip_off[L];
        int mw = s->mip_w[L], mh = s->mip_h[L];
        for (int r = ra; r < rb; r++) {
            int ty = (int)floor((scen(r) + b->yoff - c->y0) * inv);
            ty = ty < 0 ? 0 : ty >= s->h ? s->h - 1 : ty;
            ty >>= L;
            if (ty >= mh) ty = mh - 1;
            const u8 *mrow = m + 2 * ty * mw;
            u8 *drow = T->smp + (size_t)r * T->sw;
            for (int col = ca; col < cb; col++) {
                int t = tx[col] >> L;
                const u8 *e = mrow + 2 * (t < mw ? t : mw - 1);
                if (!e[1]) continue;
                float cov = e[1] * (1.0f / 255.0f) * (c->alpha < 1 ? c->alpha : 1.0f);
                if (cov < 1 && !dither_pass(cov, col, r)) continue;
                u8 v = e[0], d = enh_base[drow[col]], n = lut[v << 4 | d];
                if (n != d) drow[col] = n;
            }
        }
        return;
    }
    for (int r = ra; r < rb; r++) {
        int ty = (int)floor((scen(r) + b->yoff - c->y0) * inv);
        if (ty < 0) ty = 0;
        if (ty >= s->h) ty = s->h - 1;
        const u8 *srow = s->bits + ty * s->w;
        u8 *drow = T->smp + (size_t)r * T->sw;
        for (int col = ca; col < cb; col++) {
            u8 v = srow[tx[col]];
            if (!(touch & (1 << v))) continue;
            if (c->alpha < 1 && !dither_pass(c->alpha, col, r)) continue;
            u8 d = enh_base[drow[col]], n = lut[v << 4 | d];
            if (n != d) drow[col] = n;                    /* an extended colour stays where nothing changes */
        }
    }
}

static void do_line(const Band *b, const EnhCmd *c)
{
    float hw = c->w * 0.5f, minhw = 0.5f / (float)enh_sq;
    if (hw < minhw) hw = minhw;
    float dx = c->x1 - c->x0, dy = c->y1 - c->y0;
    float len = (float)sqrt((double)dx * dx + (double)dy * dy);
    float ux = len > 0 ? dx / len : 1, uy = len > 0 ? dy / len : 0;
    float ymin = (c->y0 < c->y1 ? c->y0 : c->y1) - hw, ymax = (c->y0 > c->y1 ? c->y0 : c->y1) + hw;
    float xmin = (c->x0 < c->x1 ? c->x0 : c->x1) - hw, xmax = (c->x0 > c->x1 ? c->x0 : c->x1) + hw;
    int ra, rb, ca, cb;
    row_range(b, clip_lo(c, ymin), clip_hi(b, c, ymax), &ra, &rb);
    col_range(b, cx_lo(c, xmin), cx_hi(c, xmax), &ca, &cb);
    for (int r = ra; r < rb; r++) {
        float py = scen(r) + b->yoff - c->y0;
        u8 *row = b->t->smp + (size_t)r * b->t->sw;
        for (int col = ca; col < cb; col++) {
            float px = scen(col) - c->x0;
            float along = px * ux + py * uy, perp = px * uy - py * ux;
            if (along < -hw || along > len + hw || perp < -hw || perp > hw) continue;
            if (c->alpha < 1 && !dither_pass(c->alpha, col, r)) continue;
            row[col] = c->colour;
        }
    }
}

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------------------------------------ */
/* new assets (ENHANCED.md "New assets")                                                            */

#define MARK_W       0.05f       /* road markings: width as a fraction of the road half-width W */
#define ALT_PERIOD   4.0         /* road pattern: two units of each shade */
#define ALT_FADE     60.0        /* road pattern: contrast 1 / (1 + z / ALT_FADE) */
/* Rock faces, drop-offs and the valley floor follow Test Drive Enhanced (ENHANCED.md "New assets"); lengths
 * in the original's px are for the 320-px front view and scaled for the mirror. */
#define EDGE_JAG     60.0        /* notches in the slanted outline of rock faces and hillsides (lateral units) */
#define JAG_FOOT     40.0        /* the outline notches fade in over this height from the road edge */
#define RIM_H        45.0        /* dark rim straight down from a drop-off edge (road height units) */
#define HILL_GRAD    400.0       /* hillside below the rim: from its top colour to the hill colour over this */
#define VALLEY_H     4000.0      /* valley floor below the eye: 50 times the eye height (80), as there */
#define VALLEY_LAT   90.0        /* lateral units per road unit (the road's half-width, 403, is 4.5 units) */
#define VALLEY_HAZE_Z 3000.0     /* valley haze 1 - exp(-z / VALLEY_HAZE_Z) (road units) */
#define VALLEY_FADE_Z 2000.0     /* valley contrast exp(-z / VALLEY_FADE_Z) */

static u32 hash32(u32 x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
static double h01(u32 x) { return (hash32(x) & 0xFFFFFF) / 16777216.0; }
static double smooth01(double t) { return t * t * (3 - 2 * t); }

/* smooth 2-D value noise in [0, 1) */
static double noise2(double x, double y, u32 seed)
{
    double fx = floor(x), fy = floor(y);
    u32 ix = (u32)(s32)fx, iy = (u32)(s32)fy;
    double tx = smooth01(x - fx), ty = smooth01(y - fy);
    double v00 = h01((ix * 0x9E3779B1u) ^ (iy * 0x85EBCA77u) ^ seed);
    double v10 = h01(((ix + 1) * 0x9E3779B1u) ^ (iy * 0x85EBCA77u) ^ seed);
    double v01 = h01((ix * 0x9E3779B1u) ^ ((iy + 1) * 0x85EBCA77u) ^ seed);
    double v11 = h01(((ix + 1) * 0x9E3779B1u) ^ ((iy + 1) * 0x85EBCA77u) ^ seed);
    double a = v00 + (v10 - v00) * tx, c = v01 + (v11 - v01) * tx;
    return a + (c - a) * ty;
}

/* Notch of a slanted outline in px at depth z, road position u, v height units away from its road edge:
 * a noise of the height and the road position (fixed to the world: it keeps its place on the rock and only
 * grows with perspective as the car approaches), faded in over JAG_FOOT above or below the edge */
static double edge_jag(const EnhScene *S, double z, double u, double v, u32 seed)
{
    if (v <= 0) return 0;
    double f = v >= JAG_FOOT ? 1 : smooth01(v / JAG_FOOT);
    double n = 0.75 * noise2(v / 150.0, u / 3.0, seed) + 0.25 * noise2(v / 50.0, u / 1.3, seed ^ 0x5CA1u);
    return EDGE_JAG * S->kx / z * f * n;
}

/* integral over [0, x] of a square wave that is 1 on [0, on) of every period */
static double sq_int(double x, double period, double on)
{
    double n = floor(x / period), m = x - n * period;
    return n * on + (m < on ? m : on);
}

/* fraction of [a, b] where that wave is 1 (a box filter; a point sample for an empty interval) */
static double sq_frac(double a, double b, double period, double on)
{
    if (b - a < 1e-6) return a - floor(a / period) * period < on ? 1 : 0;
    return (sq_int(b, period, on) - sq_int(a, period, on)) / (b - a);
}

/* depth of a ground scanline at t between the far and the near row (1/z is linear on the screen), and the
 * depth one sample row spans there */
static double scan_depth(const EnhRow *fr, const EnhRow *nr, float t, double *dz)
{
    double izf = 1.0 / fr->z, izn = 1.0 / nr->z;
    double iz = izf + (izn - izf) * t;
    if (iz < 1e-4) iz = 1e-4;
    double z = 1.0 / iz, dy = nr->y - fr->y;
    *dz = dy > 1e-6 ? z * z * fabs(izn - izf) / dy / enh_sq : 0;
    return z;
}

/* haze of rock faces, rims and hillsides at depth z, 0..1 (level 1 = HAZE_MAX): none near the car, growing
 * from about 27 units on (Test Drive Enhanced's haze, its depths scaled by the draw distances 120 / 180) */
double enh_rock_haze(double z)
{
    double a = clampd((10.0 * z - 266.0) / 1700.0, 0, 1);
    return pow(a, 0.9);
}

#define HAZE_LUT_K 8                     /* entries per unit of depth */
#define HAZE_LUT_N (HAZE_LUT_K * 200)
static float haze_lut[HAZE_LUT_N];

/* The farthest rock turns into the sky colour over the last ENH_ROCK_FADE of the drawn distance, so the end of
 * the view is rock receding into the haze and then the sky rather than a dithered curtain */
double enh_rock_end(const EnhScene *S, double z)
{
    double zlim = S->nrows + S->depth0 - 2, z0 = zlim * (1 - ENH_ROCK_FADE);
    return clampd((z - z0) / (zlim - z0), 0, 1);
}

static inline double rock_haze(double z)
{
    int i = (int)(z * HAZE_LUT_K);
    return haze_lut[i < 0 ? 0 : i >= HAZE_LUT_N ? HAZE_LUT_N - 1 : i];
}

/* level of a ramp of n colours for v in 0..1, ordered-dithered between neighbouring levels per sample */
static inline int dither_level(double v, int n, int c, int r)
{
    int l = (int)(v * (n - 1) + (BAYER[r & 3][c & 3] + 0.5) / 16.0);
    return l < 0 ? 0 : l >= n ? n - 1 : l;
}

/* Road pattern: shade level of the road and the shoulders at depth z. The shades alternate every
 * ALT_PERIOD / 2 units of the road position, box-filtered over the depth the sample row covers (a steady
 * middle shade where the stripes are thinner than a scanline) and fading with distance. */
static int shade_level(const EnhScene *S, double z, double dz)
{
    double u = S->u0 + S->uk * z;
    double f = sq_frac(u - dz / 2, u + dz / 2, ALT_PERIOD, ALT_PERIOD / 2);
    return (int)(f / (1 + z / ALT_FADE) * (ENH_SHADES - 1) + 0.5);
}

/* contrast of a pattern of period p where one output pixel spans fp: fades out before it would shimmer */
static inline double band_limit(double p, double fp) { return clampd((p / fp - 3.0) / 6.0, 0, 1); }

/* The drop-off side: the valley floor, a plane VALLEY_H below the eye, far below the road. Test Drive
 * Enhanced's fields (two octaves of smooth noise between three field colours, with woods) in ground
 * coordinates: they come towards the car as it drives and pan with the mountains when it turns. Their
 * contrast fades with distance and where a pattern is only a few pixels large, and the floor is hazed
 * towards the sky colour. Above its horizon (the road climbing) the original's sky colour. The noise is
 * evaluated once per output pixel, the levels are dithered per sample. */
static void valley_span(const Band *b, int r, float x0, float x1, float yc)
{
    const EnhScene *S = b->S;
    double dy = yc - S->horizon;
    if (dy < 0.25) {
        span(b, r, x0, x1, EXT_VOID, 1);
        return;
    }
    int ca, cb;
    col_range(b, x0, x1, &ca, &cb);
    if (ca >= cb) return;
    double zv = VALLEY_H * S->ky / dy;                              /* depth of the floor on this scanline */
    double wz = S->u0 + S->uk * zv;                                 /* its road position */
    double lk = zv / S->kx / VALLEY_LAT * S->uk;                    /* road units per px across (mirror: reversed) */
    double l0 = (S->valley_shift - S->centre) * lk;
    double fp = zv / dy, fx = fabs(lk);                             /* road units one px spans (depth, across) */
    if (fx > fp) fp = fx;
    fp /= enh_scale;                                                /* one output pixel */
    double con = exp(-zv / VALLEY_FADE_Z);
    double c1 = 0.6 * con * band_limit(173, fp), c2 = 0.4 * con * band_limit(53, fp);
    double cw = 3.0 * con * band_limit(28, fp);
    double haze = 1 - exp(-zv / VALLEY_HAZE_Z);
    u8 *row = b->t->smp + (size_t)r * b->t->sw;
    int q = enh_q;
    for (int c = ca; c < cb;) {
        int px = c / q, ce = (px + 1) * q < cb ? (px + 1) * q : cb;
        double X = l0 + ((double)px + 0.5) / enh_scale * lk;
        double n = 0.5 + (noise2(X / 173, wz / 173, 11) - 0.5) * c1 + (noise2(X / 53, wz / 53, 23) - 0.5) * c2;
        double w = cw > 0 ? (noise2(X / 28, wz / 28, 37) - 0.62) * cw : 0;
        n = clampd(n, 0, 1);
        for (; c < ce; c++) {
            int tex = w > 0 && dither_pass((float)w, c + 2, r + 1) ? 0 : 1 + dither_level(n, 7, c + 1, r + 2);
            row[c] = (u8)(EXT_VALLEY + dither_level(haze, 8, c, r) * 8 + tex);
        }
    }
}

/* The ground strip beside a drop-off, from the shoulder at xs outwards over wv px to the edge: the ground
 * colour darkening towards the rim, hazed with the scanline's depth z */
static void verge_span(const Band *b, int r, float x0, float x1, float xs, float wv, int side, double z)
{
    int ca, cb;
    col_range(b, x0, x1, &ca, &cb);
    if (ca >= cb || !(wv > 0)) return;
    u8 *row = b->t->smp + (size_t)r * b->t->sw;
    double hz = rock_haze(z);
    for (int c = ca; c < cb; c++) {
        double g = fabs(scen(c) - xs) / wv;
        row[c] = (u8)(EXT_VERGE + side * 32 + dither_level(clampd(g, 0, 1), 4, c + 3, r) * 8 + dither_level(hz, 8, c, r));
    }
}

static void do_ground(const Band *b)
{
    const EnhTarget *T = b->t;
    const EnhScene *S = b->S;
    float wd = (float)T->vw;
#define CLAMPW(v) ((v) < 0 ? 0 : (v) > wd ? wd : (v))
    int pi = 0;
    for (int r = b->r0; r < b->r1; r++) {
        float yc = scen(r) + b->yoff;
        T->g_near[r] = T->g_far[r] = -1;
        if (yc < S->top_sy || yc >= T->vh) continue;
        /* pairs run near to far with decreasing y */
        while (pi > 0 && yc >= S->pairs[pi - 1].ylo) pi--;
        while (pi < S->npairs && yc < S->pairs[pi].ylo) pi++;
        if (pi >= S->npairs || yc >= S->pairs[pi].yhi) continue;
        const EnhPair *p = &S->pairs[pi];
        const EnhRow *fr = &S->rows[p->far], *nr = &S->rows[p->near];
        float dy = nr->y - fr->y;
        float t = dy > 1e-6f ? (yc - fr->y) / dy : 0;
        float ol = lerpf(fr->ol, nr->ol, t), l = lerpf(fr->L, nr->L, t);
        float rr = lerpf(fr->R, nr->R, t), orr = lerpf(fr->or_, nr->or_, t);
        float band = lerpf(fr->band, nr->band, t);
        ol = CLAMPW(ol); l = CLAMPW(l); rr = CLAMPW(rr); orr = CLAMPW(orr); band = CLAMPW(band);
        T->g_near[r] = (s16)p->near;
        T->g_far[r] = (s16)p->far;
        T->g_t[r] = t;
        T->g_l[r] = l;
        T->g_r[r] = rr;
        u8 f = fr->state;
        float x = 0;
        double dz, z = scan_depth(fr, nr, t, &dz);
        int sl = shade_level(S, z, dz);                  /* road pattern */
        bool valley = !(f & 0x80);                       /* the drop-off side: the valley floor */
        float vw = lerpf(fr->W, nr->W, t) * VERGE_W;     /* the ground strip beside a drop-off */
#define FILL_TO(end, col) do { float e_ = (end); if (e_ > x) { span(b, r, x, e_, (u8)(col), 1); x = e_; } } while (0)
#define VOID_TO(end) do {                                                                          \
            float e_ = (end);                                                                      \
            if (e_ > x) {                                                                          \
                if (valley) valley_span(b, r, x, e_, yc);                                          \
                else span(b, r, x, e_, S->col_sky, 1);                                             \
                x = e_;                                                                            \
            }                                                                                      \
        } while (0)
#define VERGE_TO(end, xs, side) do {                                                               \
            float e_ = (end);                                                                      \
            if (e_ > x) {                                                                          \
                if (valley) verge_span(b, r, x, e_, (xs), vw, (side), z);                          \
                else span(b, r, x, e_, S->col_sky, 1);                                             \
                x = e_;                                                                            \
            }                                                                                      \
        } while (0)
        if (f & 0x20) {                                   /* left drop-off */
            VOID_TO(CLAMPW(ol - vw));
            VERGE_TO(ol, ol, 0);
        } else {
            FILL_TO(ol, S->col_left);
        }
        FILL_TO(l, EXT_SHLD + sl);
        FILL_TO(rr, EXT_ROAD + sl);
        FILL_TO(orr, EXT_SHLD + sl);
        if (f & 0x04) {                                   /* right drop-off */
            VERGE_TO(CLAMPW(orr + vw), orr, 1);
            VOID_TO(wd);
        } else {
            FILL_TO(band, S->col_right);
            FILL_TO(wd, S->col_far);
        }
#undef FILL_TO
#undef VOID_TO
#undef VERGE_TO
    }
#undef CLAMPW
}

static void do_walls(const Band *b, const EnhCmd *c)
{
    const EnhTarget *T = b->t;
    int ra, rb;
    row_range(b, clip_lo(c, c->y0), clip_hi(b, c, c->y1), &ra, &rb);
    float in_l = c->x0, in_r = c->x1;
    for (int r = ra; r < rb; r++) {
        if (T->g_far[r] < 0) continue;
        float l = T->g_l[r], rr = T->g_r[r];
        if (!b->S->style) {
            if (in_l < l) span(b, r, in_l, l, 0, 1);
            if (l < rr) span(b, r, l, rr, 8, 1);
            if (rr < in_r) span(b, r, rr, in_r, 0, 1);
        } else {
            if (in_l < l) span(b, r, in_l, l, 8, 1);
            if (rr < in_r) span(b, r, rr, in_r, 8, 1);
        }
    }
}

static void do_band(const Band *b, const EnhCmd *c)
{
    const EnhTarget *T = b->t;
    int ra, rb;
    row_range(b, clip_lo(c, c->y0), clip_hi(b, c, c->y1), &ra, &rb);
    for (int r = ra; r < rb; r++) {
        if (T->g_far[r] < 0) continue;
        if (T->g_l[r] < T->g_r[r]) span(b, r, cx_lo(c, T->g_l[r]), cx_hi(c, T->g_r[r]), c->colour, c->alpha);
    }
}

/* A rock face (Test Drive Enhanced's cliff_face) between row a (far) and row a - 1 (near): the original's
 * plain face (colour 6) above the outer road edge up to the top of the view, leaning outwards by CLIFF_LEAN
 * like its cliff-edge sprite, its slanted outline notched by a noise of the height above the road and the road
 * position (edge_jag, fixed to the world). The faces of neighbouring pairs share their edge points and the
 * notch of each, so they join without gaps; nearer pairs are drawn later. The nearest face within the
 * original's rows also covers everything outwards of it. Hazed with distance, and at the end of the drawn
 * distance turning into the sky colour (EXT_ROCK_END). */
static void do_face(const Band *b, const EnhCmd *c)
{
    const EnhScene *S = b->S;
    const EnhRow *fr = &S->rows[c->a], *nr = &S->rows[c->a - 1];
    bool left = c->op != 0, nearest = c->w > 0;
    float out = left ? -1.0f : 1.0f;
    float ea = left ? nr->ol : nr->or_, eb = left ? fr->ol : fr->or_;
    float fa = nr->y, fb = fr->y;
    int ra, rb;
    row_range(b, clip_lo(c, 0), clip_hi(b, c, fa > fb ? fa : fb), &ra, &rb);
    if (ra >= rb) return;
    double za = nr->z, zb = fr->z, iza = 1.0 / za, izb = 1.0 / zb, ky = S->ky;
    double ua = S->u0 + S->uk * za, ub = S->u0 + S->uk * zb;
    u8 *smp = b->t->smp;
    for (int r = ra; r < rb; r++) {
        float y = scen(r) + b->yoff;
        double da = fa - y, db = fb - y;                 /* height above each end's road edge (px) */
        float xa = ea + out * (float)(CLIFF_LEAN * da + edge_jag(S, za, ua, da * za / ky, 0xC11FF));
        float xb = eb + out * (float)(CLIFF_LEAN * db + edge_jag(S, zb, ub, db * zb / ky, 0xC11FF));
        float lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
        if (nearest) {
            if (left) lo = 0;
            else hi = (float)b->t->vw;
        }
        int ca, cb;
        col_range(b, cx_lo(c, lo), cx_hi(c, hi), &ca, &cb);
        if (ca >= cb) continue;
        u8 *row = smp + (size_t)r * b->t->sw;
        float dx = xb - xa, inv = fabsf(dx) > 1e-6f ? 1.0f / dx : 0;
        for (int col = ca; col < cb; col++) {
            float t = (scen(col) - xa) * inv;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            if (y > fa + (fb - fa) * t) continue;            /* below the road edge */
            if (c->alpha < 1 && !dither_pass(c->alpha, col, r)) continue;
            double z = 1.0 / (iza + (izb - iza) * t);
            double e = enh_rock_end(S, z);
            row[col] = e > 0 ? (u8)(EXT_ROCK_END + dither_level(e, ENH_ROCK_END, col, r))
                             : (u8)(EXT_ROCK + dither_level(rock_haze(z), ENH_HAZE, col, r));
        }
    }
}

/* Below a drop-off (Test Drive Enhanced's left_side), between row a (far) and row a - 1 (near): from the
 * outer edge of the ground strip beside the road a dark rim straight down (RIM_H), then the hillside
 * falling away outwards at 1:1 down to the valley floor, from dark earth into a hill colour, its outline
 * notched like the rock faces' (fixed to the world); hazed with distance. Only the drop-off side is painted (the
 * void, the valley and other rims and hillsides): the road and the strip in front of it stay, nearer pairs
 * are drawn later. On a straight road both stay under the road; in bends they carry the far road over the
 * valley. */
static void do_drop(const Band *b, const EnhCmd *c)
{
    const EnhScene *S = b->S;
    const EnhRow *fr = &S->rows[c->a], *nr = &S->rows[c->a - 1];
    bool left = c->op != 0;
    float out = left ? -1.0f : 1.0f;
    float ea = (left ? nr->ol : nr->or_) + out * nr->W * VERGE_W, eb = (left ? fr->ol : fr->or_) + out * fr->W * VERGE_W;
    float fa = nr->y, fb = fr->y;
    double iza = 1.0 / nr->z, izb = 1.0 / fr->z, ky = S->ky;
    double lean = S->kx / S->ky;                            /* a 1:1 slope on the screen */
    double ua = S->u0 + S->uk * nr->z, ub = S->u0 + S->uk * fr->z;
    /* the valley floor under each end: nothing is drawn below it */
    float va = (float)(S->horizon + VALLEY_H * ky * iza), vb = (float)(S->horizon + VALLEY_H * ky * izb);
    float top = fa < fb ? fa : fb, bot = va > vb ? va : vb;
    int ra, rb;
    row_range(b, clip_lo(c, top), clip_hi(b, c, bot), &ra, &rb);
    u8 *smp = b->t->smp;
    int sw = b->t->sw;
    for (int r = ra; r < rb; r++) {
        float y = scen(r) + b->yoff;
        u8 *row = smp + (size_t)r * sw;
        /* rim: straight down, between the edge points */
        float dx = eb - ea;
        if (fabsf(dx) > 1e-6f) {
            int ca, cb;
            col_range(b, cx_lo(c, ea < eb ? ea : eb), cx_hi(c, ea < eb ? eb : ea), &ca, &cb);
            for (int col = ca; col < cb; col++) {
                if (!enh_void[row[col]]) continue;
                float t = (scen(col) - ea) / dx;
                float f = fa + (fb - fa) * t;
                double z = 1.0 / (iza + (izb - iza) * t);
                double v = (y - f) * z / ky;                   /* height below the edge */
                if (v < 0 || v > RIM_H) continue;
                row[col] = (u8)(EXT_RIM + dither_level(rock_haze(z), ENH_HAZE, col, r));
            }
        }
        /* hillside: leaning outwards below each end, its outline notched */
        double da = y - fa, db = y - fb;
        float xa = ea + out * (float)(lean * da - edge_jag(S, nr->z, ua, da * nr->z / ky, 0x1B0A7));
        float xb = eb + out * (float)(lean * db - edge_jag(S, fr->z, ub, db * fr->z / ky, 0x1B0A7));
        dx = xb - xa;
        if (fabsf(dx) <= 1e-6f) continue;
        int ca, cb;
        col_range(b, cx_lo(c, xa < xb ? xa : xb), cx_hi(c, xa < xb ? xb : xa), &ca, &cb);
        for (int col = ca; col < cb; col++) {
            if (!enh_void[row[col]]) continue;
            float t = (scen(col) - xa) / dx;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float f = fa + (fb - fa) * t;
            if (y < f) continue;
            double z = 1.0 / (iza + (izb - iza) * t);
            double v = (y - f) * z / ky;
            double depth = (f - S->horizon) * z / ky;         /* the edge's depth below the eye */
            if (depth + v > VALLEY_H) continue;                 /* below the valley floor */
            int hl = dither_level(rock_haze(z), 8, col, r);
            if (v <= RIM_H) row[col] = (u8)(EXT_RIM + hl * 2);
            else row[col] = (u8)(EXT_HILL + dither_level(clampd((v - RIM_H) / HILL_GRAD, 0, 1), 4, col + 1, r + 2) * 8 + hl);
        }
    }
}

/* a marking strip of half-width hw at x with coverage cov (0..1): on the road colour a mix of the two
 * (EXT_MARK_*), elsewhere the marking colour, dithered by cov */
static void mark_strip(const Band *b, int r, float x, float hw, int ramp, u8 full, float cov)
{
    if (!(x + hw > 0 && x - hw < b->t->vw)) return;
    int lvl = (int)(cov * (ENH_COVER - 1) + 0.5f);
    if (lvl <= 0) return;
    int ca, cb;
    col_range(b, x - hw, x + hw, &ca, &cb);
    u8 *row = b->t->smp + (size_t)r * b->t->sw;
    for (int col = ca; col < cb; col++) {
        if (enh_base[row[col]] == 7) row[col] = (u8)(ramp + lvl);
        else if (dither_pass(cov, col, r)) row[col] = full;
    }
}

/* Road markings. The original sets one pixel per road unit: the centre line (plane 0 cleared, planes 1-3
 * set: always colour 14) where the road is wide or the dash phase is on, the lane lines (colour 15) where
 * both are. Here they are strips MARK_W times the road's half-width wide, at least one output pixel, and
 * a thinner strip is drawn in a mix with the road colour instead; the dashes are box-filtered over the
 * depth each scanline covers, so that far away, where a dash is less than a scanline deep, they turn into
 * a steady faint line instead of flickering. */
static void do_mark(const Band *b, const EnhCmd *c)
{
    const EnhTarget *T = b->t;
    const EnhScene *S = b->S;
    const EnhRow *fr = &S->rows[c->a];
    float minw = 1.0f / (float)enh_scale;
    for (int r = b->r0; r < b->r1; r++) {
        if (T->g_far[r] != c->a) continue;
        const EnhRow *nr = &S->rows[T->g_near[r]];
        float t = T->g_t[r];
        /* road unit of this scanline and the depth the sample row covers */
        double dz, z = scan_depth(fr, nr, t, &dz);
        double u = S->u0 + S->uk * z;
        /* the unit whose far edge is beyond the scanline: towards the far row */
        int n = fr->unit > nr->unit ? (int)ceil(u - 1e-9) : (int)floor(u + 1e-9);
        int lo = fr->unit < nr->unit ? fr->unit : nr->unit, hi = fr->unit < nr->unit ? nr->unit : fr->unit;
        if (n > hi) n = hi;
        if (n < lo) n = lo;
        u8 fl;
        if (n == fr->unit) fl = fr->flags;
        else if (n == nr->unit) fl = nr->flags;
        else {
            long a = 0x3B51L + n;
            u8 rb = (a >= 0x3B51 && a < 0x52C8) ? DSB((u16)a) : 0;
            fl = (u8)((rb >> 7) | DSB((u16)(DS_road_records + (rb & 0x7F) * 4)));
        }
        /* dash phase: unit n has phase fr->phase - (fr->unit - n), on while bit 2 is clear */
        double y = u + (double)fr->phase - fr->unit + (fr->unit > nr->unit ? 1 : 0);
        double dash = sq_frac(y - dz / 2, y + dz / 2, 8, 4);
        float cc = (fl & 1) ? 1.0f : (float)dash, cl = (fl & 1) ? (float)dash : 0.0f;
        if (cc <= 0) continue;
        float cx = lerpf(fr->cx, nr->cx, t) + 0.5f, W = lerpf(fr->W, nr->W, t);
        float w = W * MARK_W, cw = w / minw;
        if (cw > 1) cw = 1;
        if (w < minw) w = minw;
        mark_strip(b, r, cx, w / 2, EXT_MARK_C, 14, cc * cw);
        if (cl > 0) {
            mark_strip(b, r, cx + W, w / 2, EXT_MARK_L, 15, cl * cw);
            if (S->median) mark_strip(b, r, cx - W, w / 2, EXT_MARK_L, 15, cl * cw);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* resolve                                                                                          */

static u16 lin_of[256];                   /* sRGB byte -> linear 0..4095 */
static u8 srgb_of[4096];
static u16 pal_lin[ENH_NCOL][3];
static bool luts_ready;

/* ------------------------------------------------------------------------------------------------ */
/* extended colours                                                                                 */

u8 enh_base[ENH_NCOL];
u8 enh_void[ENH_NCOL];
static EnhMix mixes[ENH_NCOL];
static u32 mix_key;                       /* changes with the extended colours */
static int cols_cur = -1;
static u8 haze_sky = 11;                  /* the sky colour in the haze colour */

/* Colour constants (ENHANCED.md "New assets"): mixes of the stage's own colours */
#define ROAD_ALT   0.10f                  /* the alternate road shade: this much of colour 8 in colour 7 */
#define SHLD_ALT   0.22f                  /* the alternate shoulder shade: this much darker */
#define HAZE_MAX   0.6f                   /* haze of rock faces, rims and hillsides at level 1 */
#define VALLEY_HAZE 0.6f                  /* haze of the valley floor at the horizon */

static void set_mix(int i, u8 a, u8 b, float t, float k, u8 c, float h, u8 base, bool v)
{
    mixes[i] = (EnhMix){ a, b, c, t, k, h };
    enh_base[i] = base;
    enh_void[i] = v;
}

/* w2 of colour 2 and w6 of colour 6 (linear light) */
static void set_w26(int i, float w2, float w6, u8 c, float h, u8 base, bool v)
{
    float k = w2 + w6;
    set_mix(i, 2, 6, k > 0 ? w6 / k : 0, k, c, h, base, v);
}

void enh_colours_setup(u8 col_left, u8 col_right, u8 col_shoulder, u8 col_sky)
{
    int key = col_left | col_right << 4 | col_shoulder << 8 | col_sky << 12;
    if (key == cols_cur) return;
    cols_cur = key;
    haze_sky = col_sky;
    for (int i = 0; i < ENH_NCOL; i++) set_mix(i, (u8)(i & 15), 0, 0, 1, 0, 0, (u8)(i & 15), false);
    for (int l = 0; l < ENH_SHADES; l++) {
        float f = (float)l / (ENH_SHADES - 1);
        set_mix(EXT_ROAD + l, 7, 8, ROAD_ALT * f, 1, 0, 0, 7, false);
        set_mix(EXT_SHLD + l, col_shoulder, col_shoulder, 0, 1 - SHLD_ALT * f, 0, 0, col_shoulder, false);
    }
    for (int l = 0; l < ENH_COVER; l++) {
        float f = (float)l / (ENH_COVER - 1);
        set_mix(EXT_MARK_C + l, 7, 14, f, 1, 0, 0, f < 0.5f ? 7 : 14, false);
        set_mix(EXT_MARK_L + l, 7, 15, f, 1, 0, 0, f < 0.5f ? 7 : 15, false);
    }
    for (int l = 0; l < ENH_HAZE; l++) {
        float h = HAZE_MAX * (float)l / (ENH_HAZE - 1);
        set_mix(EXT_ROCK + l, 6, 6, 0, 1, ENH_HAZE_COL, h, 6, false);
        set_mix(EXT_RIM + l, 6, 8, 0.7f, 0.12f, ENH_HAZE_COL, h, 6, true);          /* dark earth */
    }
    /* Test Drive Enhanced's colours as mixes of green (2) and brown (6): the hillside from its top colour
     * into the hill colour, the valley's woods and its three field colours */
    for (int g = 0; g < 4; g++)
        for (int l = 0; l < 8; l++) {
            float f = (float)g / 3, h = HAZE_MAX * (float)l / 7;
            set_w26(EXT_HILL + g * 8 + l, 0.108f + (0.29f - 0.108f) * f, 0.162f + (0.29f - 0.162f) * f, ENH_HAZE_COL, h, 6, true);
        }
    /* the ground strip beside a drop-off: the ground colour of that side darkening towards the edge */
    for (int side = 0; side < 2; side++)
        for (int g = 0; g < 4; g++)
            for (int l = 0; l < 8; l++) {
                u8 gc = side ? col_right : col_left;
                float f = (float)g / 3, h = HAZE_MAX * (float)l / 7;
                set_mix(EXT_VERGE + side * 32 + g * 8 + l, gc, 8, 0.3f * f, 1 - 0.65f * f, ENH_HAZE_COL, h, gc, false);
            }
    static const float field[3][2] = { { 0.44f, 0.20f }, { 0.68f, 0.53f }, { 0.49f, 0.80f } };   /* w2, w6 */
    for (int l = 0; l < 8; l++) {
        float h = VALLEY_HAZE * (float)l / 7;
        set_w26(EXT_VALLEY + l * 8, 0.21f, 0.07f, ENH_HAZE_COL, h, col_sky, true);                  /* woods */
        for (int v = 1; v < 8; v++) {
            float f = (float)(v - 1) / 6 * 2;                                                     /* 0..2 */
            int k = f < 1 ? 0 : 1;
            float u = f - (float)k;
            set_w26(EXT_VALLEY + l * 8 + v, field[k][0] + (field[k + 1][0] - field[k][0]) * u,
                    field[k][1] + (field[k + 1][1] - field[k][1]) * u, ENH_HAZE_COL, h, col_sky, true);
        }
    }
    set_mix(EXT_VOID, col_sky, col_sky, 0, 1, 0, 0, col_sky, true);
    for (int l = 0; l < ENH_ROCK_END; l++) {             /* fully hazed rock -> the sky colour */
        float f = (float)l / (ENH_ROCK_END - 1);
        set_mix(EXT_ROCK_END + l, 6, col_sky, f, 1, ENH_HAZE_COL, HAZE_MAX * (1 - f), 6, false);
    }
    mix_key++;
}

static void init_luts(void)
{
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        lin_of[i] = (u16)(c * 4095.0 + 0.5);
    }
    for (int i = 0; i < 4096; i++) {
        double c = i / 4095.0;
        c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
        srgb_of[i] = (u8)(c * 255.0 + 0.5);
    }
    for (int i = 0; i < HAZE_LUT_N; i++) haze_lut[i] = (float)enh_rock_haze((i + 0.5) / HAZE_LUT_K);
    luts_ready = true;
}

u32 enh_palette_key(void)
{
    u32 h = 2166136261u;
    for (int i = 0; i < 16; i++) {
        h ^= gfx_palette_rgb((u8)i);
        h *= 16777619u;
    }
    h ^= mix_key;
    h *= 16777619u;
    return h;
}

static void resolve_rows(const EnhTarget *T, int o0, int o1)
{
    int q = enh_q;
    u32 n = (u32)(q * q), half = n / 2;
    for (int oy = o0; oy < o1; oy++) {
        u32 *dst = T->out + (size_t)oy * T->ow;
        for (int ox = 0; ox < T->ow; ox++) {
            u32 r = 0, g = 0, bl = 0;
            for (int j = 0; j < q; j++) {
                const u8 *p = T->smp + (size_t)(oy * q + j) * T->sw + (size_t)ox * q;
                for (int i = 0; i < q; i++) {
                    const u16 *c = pal_lin[p[i]];
                    r += c[0];
                    g += c[1];
                    bl += c[2];
                }
            }
            dst[ox] = (u32)srgb_of[(r + half) / n] << 16 | (u32)srgb_of[(g + half) / n] << 8
                      | srgb_of[(bl + half) / n];
        }
    }
}

static void prepare_palette(EnhTarget *t)
{
    if (!luts_ready) init_luts();
    if (cols_cur < 0) enh_colours_setup(6, 6, 8, 11);
    for (int i = 0; i < 16; i++) {
        u32 c = gfx_palette_rgb((u8)i);
        pal_lin[i][0] = lin_of[c >> 16 & 255];
        pal_lin[i][1] = lin_of[c >> 8 & 255];
        pal_lin[i][2] = lin_of[c & 255];
    }
    /* the haze colour: Test Drive Enhanced's pale haze, built from the sky colour, white and light grey */
    float haze[3];
    for (int ch = 0; ch < 3; ch++)
        haze[ch] = 0.3f * pal_lin[haze_sky][ch] + 0.4f * pal_lin[15][ch] + 0.3f * pal_lin[7][ch];
    for (int i = 16; i < ENH_NCOL; i++) {
        const EnhMix *m = &mixes[i];
        for (int ch = 0; ch < 3; ch++) {
            float v = ((1 - m->t) * pal_lin[m->a][ch] + m->t * pal_lin[m->b][ch]) * m->k;
            float hc = m->c == ENH_HAZE_COL ? haze[ch] : pal_lin[m->c][ch];
            v += (hc - v) * m->h;
            pal_lin[i][ch] = (u16)(v < 0 ? 0 : v > 4095 ? 4095 : v + 0.5f);
        }
    }
    t->pal_key = enh_palette_key();
}

/* ------------------------------------------------------------------------------------------------ */

static void render_band(int i, void *ctx)
{
    const EnhTarget *T = ctx;
    const EnhScene *S = T->sc;
    Band b = { T, S, T->band_o0[i] * enh_q, T->band_o0[i + 1] * enh_q, i, 0 };
    memset(T->smp + (size_t)b.r0 * T->sw, 0, (size_t)(b.r1 - b.r0) * T->sw);
    for (int r = b.r0; r < b.r1; r++) T->g_near[r] = T->g_far[r] = -1;
    for (int k = 0; k < S->ncmds; k++) {
        const EnhCmd *c = &S->cmds[k];
        b.yoff = c->noshift ? 0 : S->yoff;
        switch (c->type) {
        case CMD_FILL:   do_fill(&b, c); break;
        case CMD_SPRITE: do_sprite(&b, c); break;
        case CMD_LINE:   do_line(&b, c); break;
        case CMD_GROUND: do_ground(&b); break;
        case CMD_WALLS:  do_walls(&b, c); break;
        case CMD_BAND:   do_band(&b, c); break;
        case CMD_MARK:   do_mark(&b, c); break;
        case CMD_FACE:   do_face(&b, c); break;
        case CMD_DROP:   do_drop(&b, c); break;
        default: break;
        }
    }
    resolve_rows(T, T->band_o0[i], T->band_o0[i + 1]);
}

void enh_raster_render(EnhTarget *t)
{
    prepare_palette(t);
    host_parallel_for(t->nbands, render_band, t);
}

static void resolve_band(int i, void *ctx)
{
    const EnhTarget *T = ctx;
    resolve_rows(T, T->band_o0[i], T->band_o0[i + 1]);
}

void enh_resolve(EnhTarget *t)
{
    prepare_palette(t);
    host_parallel_for(t->nbands, resolve_band, t);
}
