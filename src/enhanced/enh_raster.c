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
    for (int i = 0; i < ncache; i++) free(cache[i].bits);
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
#define JAG_DEPTH    0.22        /* rock face notches: deepest notch as a fraction of the face height */
#define JAG_IN       15.0        /* notches grow in over this many units beyond the original's rows */
#define VALLEY_H     480.0       /* valley floor below the eye (road height units; the road is 80 below) */
#define VALLEY_CELL_U 20.0       /* valley fields: depth in road units */
#define VALLEY_CELL_X 800.0      /*                width in lateral units (the road's half-width is 403) */
#define VALLEY_HAZE_Z 700.0      /* valley haze 1 - exp(-z / VALLEY_HAZE_Z) */
#define RIM_H        30.0        /* dark rim under a drop-off edge (road height units) */
#define HILL_LEAN    0.8         /* hillside below the rim: px outwards per px down */

static u32 hash32(u32 x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
static double h01(u32 x) { return (hash32(x) & 0xFFFFFF) / 16777216.0; }

/* smooth 1-D value noise in [0, 1) */
static double noise1(double u, u32 seed)
{
    double fl = floor(u), t = u - fl;
    u32 a = (u32)(s32)fl;
    double va = h01(a * 0x9E3779B1u + seed), vb = h01((a + 1) * 0x9E3779B1u + seed);
    t = t * t * (3 - 2 * t);
    return va + (vb - va) * t;
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

/* haze of a rock face at depth z, 0..1: none up to the original's rows (where its cut-line fill is the
 * face), HAZE_MAX (level 1) at the end of the view */
double enh_rock_haze(const EnhScene *S, double z)
{
    double zmax = S->nrows + S->depth0;
    return clampd((z - S->haze_z0) / (zmax - S->haze_z0), 0, 1);
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

/* The drop-off side below the valley's horizon: fields on a plane VALLEY_H below the eye, fixed to the
 * ground (they come towards the car as it drives and pan with the mountains when it turns), their
 * contrast fading where a field is only a few pixels deep, hazed with distance. Above that horizon (the
 * road climbing) the original's sky colour. */
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
    double zv = VALLEY_H * S->ky / dy;
    double u = S->u0 + S->uk * zv;
    double haze = 1 - exp(-zv / VALLEY_HAZE_Z);
    /* fields, and patches a third of their size in them; each fades to the mean where it is less than a
     * few pixels deep */
    double field_px = VALLEY_CELL_U * VALLEY_H * S->ky / (zv * zv);   /* a field's depth on the screen */
    double con = clampd((field_px - 1.5) / 4.0, 0, 1), con2 = clampd((field_px / 3 - 1.5) / 4.0, 0, 1);
    s32 cu = (s32)floor(u / VALLEY_CELL_U), cu2 = (s32)floor(u * 3 / VALLEY_CELL_U);
    double xoff = h01((u32)cu * 0x51ED27u + 7);                        /* fields of a row are offset */
    double kx = zv / S->kx / VALLEY_CELL_X, x_0 = xoff + (S->valley_shift - S->centre) * kx;
    u8 *row = b->t->smp + (size_t)r * b->t->sw;
    for (int c = ca; c < cb; c++) {
        double X = x_0 + scen(c) * kx;
        s32 cx = (s32)floor(X), cx2 = (s32)floor(X * 3);
        double v = 0.5 + (h01((u32)cu * 0x9E3779B1u ^ (u32)cx * 0x85EBCA77u) - 0.5) * con
                   + (h01((u32)cu2 * 0x2545F491u ^ (u32)cx2 * 0x6C8E9CF5u) - 0.5) * 0.35 * con2;
        row[c] = (u8)(EXT_VALLEY + dither_level(haze, 8, c, r) * 8 + dither_level(clampd(v, 0, 1), 8, c + 1, r + 2));
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
#define FILL_TO(end, col) do { float e_ = (end); if (e_ > x) { span(b, r, x, e_, (u8)(col), 1); x = e_; } } while (0)
#define VOID_TO(end) do {                                                                          \
            float e_ = (end);                                                                      \
            if (e_ > x) {                                                                          \
                if (valley) valley_span(b, r, x, e_, yc);                                          \
                else span(b, r, x, e_, S->col_sky, 1);                                             \
                x = e_;                                                                            \
            }                                                                                      \
        } while (0)
        if (f & 0x20) {                                   /* left drop-off */
            if (yc >= S->left_sky_y && S->left_sky_x < ol) {
                VOID_TO(S->left_sky_x);
                FILL_TO(ol, S->col_left);
            } else {
                VOID_TO(ol);
            }
        } else {
            FILL_TO(ol, S->col_left);
        }
        FILL_TO(l, EXT_SHLD + sl);
        FILL_TO(rr, EXT_ROAD + sl);
        FILL_TO(orr, EXT_SHLD + sl);
        if (f & 0x04) {                                   /* right drop-off */
            if (yc >= S->right_sky_y && S->right_sky_x >= orr) FILL_TO(S->right_sky_x, S->col_right);
            VOID_TO(wd);
        } else {
            FILL_TO(band, S->col_right);
            FILL_TO(wd, S->col_far);
        }
#undef FILL_TO
#undef VOID_TO
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

/* A rock face beyond the original's rows, between row a (far) and row a - 1 (near): the original's plain
 * face (colour 6) leaning outwards by CLIFF_LEAN like its cliff-edge sprite, from a little below the road
 * edge up to the face height of each row (the whole view at the last of the original's rows, where its
 * cut-line fill takes over, settling towards the horizon beyond). The top edge is notched by a noise fixed
 * to the road (it comes towards the car with it), growing in over JAG_IN units beyond the original's rows
 * so the face still meets the fill without a step; the colour is hazed with distance. Each scanline covers
 * the face between the two rows' edge points: neighbouring pairs share their points, so the faces of all
 * rows join without gaps, and nearer pairs are drawn later. */
static void do_face(const Band *b, const EnhCmd *c)
{
    const EnhScene *S = b->S;
    const EnhRow *fr = &S->rows[c->a], *nr = &S->rows[c->a - 1];
    bool left = c->op != 0;
    float out = left ? -1.0f : 1.0f;
    float ea = left ? nr->ol : nr->or_, eb = left ? fr->ol : fr->or_;
    float fa = nr->y, fb = fr->y, ha = c->x1, hb = c->x0;
    float top = fa - ha < fb - hb ? fa - ha : fb - hb;
    float bot = (fa > fb ? fa : fb) + (ha > hb ? ha : hb) * (float)CLIFF_FOOT;
    int ra, rb;
    row_range(b, clip_lo(c, top), clip_hi(b, c, bot), &ra, &rb);
    if (ra >= rb) return;
    double iza = 1.0 / nr->z, izb = 1.0 / fr->z;
    double ua = S->u0 + S->uk * nr->z, ub = S->u0 + S->uk * fr->z;
    double ja = clampd((nr->z - S->haze_z0) / JAG_IN, 0, 1), jb = clampd((fr->z - S->haze_z0) / JAG_IN, 0, 1);
    u8 *smp = b->t->smp;
    for (int r = ra; r < rb; r++) {
        float y = scen(r) + b->yoff;
        /* the face's outer edge at this scanline leans out above each foot, stands straight below it */
        float da = fa - y, db = fb - y;
        float xa = ea + out * (float)CLIFF_LEAN * (da > 0 ? da : 0), xb = eb + out * (float)CLIFF_LEAN * (db > 0 ? db : 0);
        float dx = xb - xa;
        int ca, cb;
        col_range(b, cx_lo(c, xa < xb ? xa : xb), cx_hi(c, xa < xb ? xb : xa), &ca, &cb);
        if (ca >= cb) continue;
        u8 *row = smp + (size_t)r * b->t->sw;
        float inv = fabsf(dx) > 1e-6f ? 1.0f / dx : 0;
        for (int col = ca; col < cb; col++) {
            float t = inv != 0 ? (scen(col) - xa) * inv : 0.5f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float f = fa + (fb - fa) * t, h = ha + (hb - ha) * t;
            if (y > f + h * (float)CLIFF_FOOT) continue;
            if (y < f - h) continue;
            if (y < f - h * (float)(1 - JAG_DEPTH)) {    /* in reach of the notches */
                double u = ua + (ub - ua) * t;
                double jag = JAG_DEPTH * (ja + (jb - ja) * t)
                             * (0.75 * noise1(u / 3.0, 0xC11F) + 0.25 * noise1(u / 1.2, 0x5CA1));
                if (y < f - h * (1 - jag)) continue;
            }
            if (c->alpha < 1 && !dither_pass(c->alpha, col, r)) continue;
            double z = 1.0 / (iza + (izb - iza) * t);
            row[col] = (u8)(EXT_ROCK + dither_level(enh_rock_haze(S, z), ENH_HAZE, col, r));
        }
    }
}

/* Below a drop-off edge between row a (far) and row a - 1 (near): a dark rim straight down from the edge
 * (RIM_H), then the hillside falling away outwards (HILL_LEAN) down to the valley floor, its colour turning
 * from dark earth into the ground colour, hazed with distance. Only the drop-off side is painted (the void,
 * the valley and other rims and hillsides): the road in front of it stays, nearer pairs are drawn later.
 * On a straight road both stay under the road; in bends they carry the far road over the valley. */
static void do_drop(const Band *b, const EnhCmd *c)
{
    const EnhScene *S = b->S;
    const EnhRow *fr = &S->rows[c->a], *nr = &S->rows[c->a - 1];
    bool left = c->op != 0;
    float out = left ? -1.0f : 1.0f;
    float ea = left ? nr->ol : nr->or_, eb = left ? fr->ol : fr->or_;
    float fa = nr->y, fb = fr->y;
    double iza = 1.0 / nr->z, izb = 1.0 / fr->z, ky = S->ky;
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
                row[col] = (u8)(EXT_RIM + dither_level(enh_rock_haze(S, z), ENH_HAZE, col, r));
            }
        }
        /* hillside: leaning outwards below each end */
        float da = y - fa, db = y - fb;
        if (da < 0) da = 0;
        if (db < 0) db = 0;
        float xa = ea + out * (float)HILL_LEAN * da, xb = eb + out * (float)HILL_LEAN * db;
        dx = xb - xa;
        if (fabsf(dx) <= 1e-6f) continue;
        int ca, cb;
        col_range(b, cx_lo(c, xa < xb ? xa : xb), cx_hi(c, xa < xb ? xb : xa), &ca, &cb);
        for (int col = ca; col < cb; col++) {
            if (!enh_void[row[col]]) continue;
            float t = (scen(col) - xa) / dx;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float f = fa + (fb - fa) * t;
            double z = 1.0 / (iza + (izb - iza) * t);
            double v = (y - f) * z / ky;
            if (v <= RIM_H) continue;
            double depth = (f - S->horizon) * z / ky;         /* the edge's depth below the eye */
            double g = (v - RIM_H) / (VALLEY_H - depth - RIM_H);
            if (g > 1) continue;                               /* below the valley floor */
            int gl = g < 0.12 ? 0 : g < 0.35 ? 1 : g < 0.65 ? 2 : 3;
            row[col] = (u8)(EXT_HILL + gl * 8 + dither_level(enh_rock_haze(S, z), 8, col, r));
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

/* Colour constants (ENHANCED.md "New assets"): mixes of the stage's own colours */
#define ROAD_ALT   0.10f                  /* the alternate road shade: this much of colour 8 in colour 7 */
#define SHLD_ALT   0.22f                  /* the alternate shoulder shade: this much darker */
#define HAZE_MAX   0.4f                   /* haze of rock faces at the end of the draw distance */
#define VALLEY_HAZE 0.85f                 /* haze of the valley floor at the horizon */

static void set_mix(int i, u8 a, u8 b, float t, float k, u8 c, float h, u8 base, bool v)
{
    mixes[i] = (EnhMix){ a, b, c, t, k, h };
    enh_base[i] = base;
    enh_void[i] = v;
}

void enh_colours_setup(u8 col_left, u8 col_right, u8 col_shoulder, u8 col_sky)
{
    int key = col_left | col_right << 4 | col_shoulder << 8 | col_sky << 12;
    if (key == cols_cur) return;
    cols_cur = key;
    (void)col_right;
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
        set_mix(EXT_ROCK + l, 6, 6, 0, 1, col_sky, h, 6, false);
        set_mix(EXT_RIM + l, 6, 0, 0.55f, 0.8f, col_sky, h, 6, true);
    }
    /* hillside: dark earth under the rim, turning into the stage's ground colour lower down; the valley
     * floor: fields between the ground colour and green (brown where the ground is green) */
    u8 g2 = col_left == 2 || col_left == 10 ? 6 : 2;
    for (int g = 0; g < 4; g++)
        for (int l = 0; l < 8; l++) {
            float h = HAZE_MAX * (float)l / 7;
            set_mix(EXT_HILL + g * 8 + l, 6, col_left, 0.25f + 0.25f * g, 0.62f + 0.1f * g, col_sky, h, col_left, true);
        }
    for (int l = 0; l < 8; l++)
        for (int v = 0; v < 8; v++) {
            float h = VALLEY_HAZE * (float)l / 7, f = (float)v / 7;
            set_mix(EXT_VALLEY + l * 8 + v, col_left, g2, 0.1f + 0.8f * f, 0.5f + 0.45f * f, col_sky, h, col_sky, true);
        }
    set_mix(EXT_VOID, col_sky, col_sky, 0, 1, 0, 0, col_sky, true);
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
    for (int i = 16; i < ENH_NCOL; i++) {
        const EnhMix *m = &mixes[i];
        for (int ch = 0; ch < 3; ch++) {
            float v = ((1 - m->t) * pal_lin[m->a][ch] + m->t * pal_lin[m->b][ch]) * m->k;
            v += (pal_lin[m->c][ch] - v) * m->h;
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
