/* Enhanced renderer: sprite decoding, the indexed sample buffer and its rasterisation (ENHANCED.md
 * "Pixels").
 *
 * The sample buffer holds palette indices at enh_sq samples per original pixel. Every display-list entry is
 * applied in order with the original operation: fills and spans set a colour, sprites combine the colour
 * with their stored planes through the blitters' AND / OR / XOR / replace rules. Rendering runs in
 * horizontal bands on the host worker pool; each band also resolves its output rows through the current
 * palette (average of the samples in linear light). */
#include "enh_internal.h"
#include "../host.h"
#include "../platform/gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int enh_scale, enh_q, enh_sq;
int enh_sw, enh_sh, enh_ow, enh_oh;
u32 *enh_out;

static u8 *smp;                           /* enh_sw x enh_sh palette indices */
static s16 *g_near, *g_far;               /* per sample row: ground pair (-1: none) */
static float *g_t, *g_l, *g_r;            /* per sample row: interpolation, clamped road edges */
static int *tx_buf;                       /* per band: texel column of each sample column */
#define MAX_BANDS 64
static int nbands, band_o0[MAX_BANDS + 1];

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

bool enh_raster_setup(void)
{
    int k = gfx_output_scale();
    if (k == enh_scale && smp) return true;
    free(smp); free(g_near); free(g_far); free(g_t); free(g_l); free(g_r); free(tx_buf); free(enh_out);
    enh_scale = k;
    enh_q = k == 1 ? 4 : 2;
    enh_sq = k * enh_q;
    enh_sw = VIEW_W * enh_sq;
    enh_sh = VIEW_H * enh_sq;
    enh_ow = VIEW_W * k;
    enh_oh = VIEW_H * k;
    smp = malloc((size_t)enh_sw * enh_sh);
    g_near = malloc((size_t)enh_sh * sizeof *g_near);
    g_far = malloc((size_t)enh_sh * sizeof *g_far);
    g_t = malloc((size_t)enh_sh * sizeof *g_t);
    g_l = malloc((size_t)enh_sh * sizeof *g_l);
    g_r = malloc((size_t)enh_sh * sizeof *g_r);
    tx_buf = malloc((size_t)MAX_BANDS * (size_t)enh_sw * sizeof *tx_buf);
    enh_out = calloc((size_t)enh_ow * enh_oh, sizeof *enh_out);
    if (!smp || !g_near || !g_far || !g_t || !g_l || !g_r || !tx_buf || !enh_out) {
        enh_scale = 0;
        return false;
    }
    nbands = enh_oh / 4 < MAX_BANDS ? enh_oh / 4 : MAX_BANDS;
    if (nbands < 1) nbands = 1;
    for (int i = 0; i <= nbands; i++) band_o0[i] = enh_oh * i / nbands;
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

typedef struct { int r0, r1, band; } Band;

static void row_range(const Band *b, float y0, float y1, int *ra, int *rb)
{
    int a = sidx(y0), e = sidx(y1);
    if (a < b->r0) a = b->r0;
    if (e > b->r1) e = b->r1;
    *ra = a;
    *rb = e;
}

static void col_range(float x0, float x1, int *ca, int *cb)
{
    if (x0 < 0) x0 = 0;
    if (x1 > VIEW_W) x1 = VIEW_W;
    int a = sidx(x0), e = sidx(x1);
    if (a < 0) a = 0;
    if (e > enh_sw) e = enh_sw;
    *ca = a;
    *cb = e;
}

static void span(int r, float x0, float x1, u8 colour, float alpha)
{
    int ca, cb;
    col_range(x0, x1, &ca, &cb);
    if (ca >= cb) return;
    u8 *row = smp + (size_t)r * enh_sw;
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
static float clip_hi(const EnhCmd *c, float y) { float v = c->cy1 < y ? c->cy1 : y; return v < VIEW_H ? v : VIEW_H; }

static float cx_lo(const EnhCmd *c, float x) { return c->cx0 > x ? c->cx0 : x; }
static float cx_hi(const EnhCmd *c, float x) { return c->cx1 < x ? c->cx1 : x; }

static void do_fill(const Band *b, const EnhCmd *c)
{
    int ra, rb;
    row_range(b, clip_lo(c, c->y0), clip_hi(c, c->y1), &ra, &rb);
    for (int r = ra; r < rb; r++) span(r, cx_lo(c, c->x0), cx_hi(c, c->x1), c->colour, c->alpha);
}

static void do_sprite(const Band *b, const EnhCmd *c)
{
    const EnhSprite *s = c->spr;
    float k = c->x1;
    float xe = c->x0 + (float)s->w * k, ye = c->y0 + (float)s->h * k;
    int ra, rb, ca, cb;
    row_range(b, clip_lo(c, c->y0), clip_hi(c, ye), &ra, &rb);
    if (ra >= rb) return;
    col_range(cx_lo(c, c->x0), cx_hi(c, xe), &ca, &cb);
    if (ca >= cb) return;
    const u8 *lut = s->lut[c->op];
    u16 touch = s->touch[c->op];
    int *tx = tx_buf + (size_t)b->band * enh_sw;
    float inv = 1.0f / k;
    for (int col = ca; col < cb; col++) {
        int t = (int)floor((scen(col) - c->x0) * inv);
        tx[col] = t < 0 ? 0 : t >= s->w ? s->w - 1 : t;
    }
    for (int r = ra; r < rb; r++) {
        int ty = (int)floor((scen(r) - c->y0) * inv);
        if (ty < 0) ty = 0;
        if (ty >= s->h) ty = s->h - 1;
        const u8 *srow = s->bits + ty * s->w;
        u8 *drow = smp + (size_t)r * enh_sw;
        for (int col = ca; col < cb; col++) {
            u8 v = srow[tx[col]];
            if (!(touch & (1 << v))) continue;
            if (c->alpha < 1 && !dither_pass(c->alpha, col, r)) continue;
            drow[col] = lut[v << 4 | drow[col]];
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
    row_range(b, clip_lo(c, ymin), clip_hi(c, ymax), &ra, &rb);
    col_range(cx_lo(c, xmin), cx_hi(c, xmax), &ca, &cb);
    for (int r = ra; r < rb; r++) {
        float py = scen(r) - c->y0;
        u8 *row = smp + (size_t)r * enh_sw;
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
static inline float clampw(float v) { return v < 0 ? 0 : v > VIEW_W ? VIEW_W : v; }

static void do_ground(const Band *b)
{
    const EnhScene *S = &enh_sc;
    int pi = 0;
    for (int r = b->r0; r < b->r1; r++) {
        float yc = scen(r);
        g_near[r] = g_far[r] = -1;
        if (yc < S->top_sy) continue;
        /* pairs run near to far with decreasing y */
        while (pi > 0 && yc >= S->pairs[pi - 1].ylo) pi--;
        while (pi < S->npairs && yc < S->pairs[pi].ylo) pi++;
        if (pi >= S->npairs || yc >= S->pairs[pi].yhi) continue;
        const EnhPair *p = &S->pairs[pi];
        const EnhRow *fr = &S->rows[p->far], *nr = &S->rows[p->near];
        float dy = nr->y - fr->y;
        float t = dy > 1e-6f ? (yc - fr->y) / dy : 0;
        float ol = clampw(lerpf(fr->ol, nr->ol, t)), l = clampw(lerpf(fr->L, nr->L, t));
        float rr = clampw(lerpf(fr->R, nr->R, t)), orr = clampw(lerpf(fr->or_, nr->or_, t));
        float band = clampw(lerpf(fr->band, nr->band, t));
        g_near[r] = (s16)p->near;
        g_far[r] = (s16)p->far;
        g_t[r] = t;
        g_l[r] = l;
        g_r[r] = rr;
        u8 f = fr->state;
        float x = 0;
#define FILL_TO(end, col) do { float e_ = (end); if (e_ > x) { span(r, x, e_, (u8)(col), 1); x = e_; } } while (0)
        u8 c = S->col_left;
        if (f & 0x20) {                                   /* left drop-off */
            c = S->col_sky;
            if (yc >= S->left_sky_y && S->left_sky_x < ol) {
                FILL_TO(S->left_sky_x, S->col_sky);
                c = S->col_left;
            }
        }
        FILL_TO(ol, c);
        FILL_TO(l, S->col_shoulder);
        FILL_TO(rr, 7);
        FILL_TO(orr, S->col_shoulder);
        if (f & 0x04) {                                   /* right drop-off */
            if (yc >= S->right_sky_y && S->right_sky_x >= orr) FILL_TO(S->right_sky_x, S->col_right);
            FILL_TO(VIEW_W, S->col_sky);
        } else {
            FILL_TO(band, S->col_right);
            FILL_TO(VIEW_W, S->col_far);
        }
#undef FILL_TO
    }
}

static void do_walls(const Band *b, const EnhCmd *c)
{
    const EnhScene *S = &enh_sc;
    int ra, rb;
    row_range(b, clip_lo(c, c->y0), clip_hi(c, c->y1), &ra, &rb);
    float in_l = c->x0, in_r = c->x1;
    for (int r = ra; r < rb; r++) {
        if (g_far[r] < 0) continue;
        float l = g_l[r], rr = g_r[r];
        if (!S->style) {
            if (in_l < l) span(r, in_l, l, 0, 1);
            if (l < rr) span(r, l, rr, 8, 1);
            if (rr < in_r) span(r, rr, in_r, 0, 1);
        } else {
            if (in_l < l) span(r, in_l, l, 8, 1);
            if (rr < in_r) span(r, rr, in_r, 8, 1);
        }
    }
}

static void do_band(const Band *b, const EnhCmd *c)
{
    int ra, rb;
    row_range(b, clip_lo(c, c->y0), clip_hi(c, c->y1), &ra, &rb);
    for (int r = ra; r < rb; r++) {
        if (g_far[r] < 0) continue;
        if (g_l[r] < g_r[r]) span(r, cx_lo(c, g_l[r]), cx_hi(c, g_r[r]), c->colour, c->alpha);
    }
}

static void mark_strip(int r, float x, float hw, u8 and_m, u8 or_m)
{
    if (!(x + hw > 0 && x - hw < VIEW_W)) return;
    int ca, cb;
    col_range(x - hw, x + hw, &ca, &cb);
    u8 *row = smp + (size_t)r * enh_sw;
    for (int col = ca; col < cb; col++) row[col] = (u8)((row[col] & and_m) | or_m);
}

static void do_mark(const Band *b, const EnhCmd *c)
{
    const EnhScene *S = &enh_sc;
    const EnhRow *fr = &S->rows[c->a];
    float minhw = 0.5f / (float)enh_sq;
    for (int r = b->r0; r < b->r1; r++) {
        if (g_far[r] != c->a) continue;
        const EnhRow *nr = &S->rows[g_near[r]];
        float t = g_t[r];
        /* road unit of this scanline (perspective: 1/z is linear on the screen) */
        double iz = 1.0 / fr->z + (1.0 / nr->z - 1.0 / fr->z) * t;
        double z = 1.0 / iz;
        double du = fr->z - nr->z;
        double u = fr->unit - (du > 1e-9 ? (fr->z - z) / du * (fr->unit - nr->unit) : 0);
        int n = (int)ceil(u - 1e-9);
        if (n > fr->unit) n = fr->unit;
        if (n < nr->unit) n = nr->unit;
        u8 ph = (u8)(fr->phase - (fr->unit - n));
        u8 fl;
        if (n == fr->unit) fl = fr->flags;
        else if (n == nr->unit) fl = nr->flags;
        else {
            long a = 0x3B51L + n;
            u8 rb = (a >= 0x3B51 && a < 0x52C8) ? DSB((u16)a) : 0;
            fl = (u8)((rb >> 7) | DSB((u16)(DS_road_records + (rb & 0x7F) * 4)));
        }
        bool dash = !(ph & 4);
        if (!((fl & 1) || dash)) continue;
        float cx = lerpf(fr->cx, nr->cx, t) + 0.5f, W = lerpf(fr->W, nr->W, t);
        float hw = (W >= 19 ? 1.0f : W / 19.0f) * 0.5f;
        if (hw < minhw) hw = minhw;
        mark_strip(r, cx, hw, (u8)~1, 0x0E);       /* plane 0 cleared, planes 1-3 set */
        if ((fl & 1) && dash) {
            mark_strip(r, cx + W, hw, 0xFF, 0x0F);
            if (S->median) mark_strip(r, cx - W, hw, 0xFF, 0x0F);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* resolve                                                                                          */

static u16 lin_of[256];                   /* sRGB byte -> linear 0..4095 */
static u8 srgb_of[4096];
static u16 pal_lin[16][3];
static u32 pal_key_used;
static bool luts_ready;

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
    return h;
}

u32 enh_resolved_palette_key(void) { return pal_key_used; }

static void resolve_rows(int o0, int o1)
{
    int q = enh_q;
    u32 n = (u32)(q * q), half = n / 2;
    for (int oy = o0; oy < o1; oy++) {
        u32 *dst = enh_out + (size_t)oy * enh_ow;
        for (int ox = 0; ox < enh_ow; ox++) {
            u32 r = 0, g = 0, bl = 0;
            for (int j = 0; j < q; j++) {
                const u8 *p = smp + (size_t)(oy * q + j) * enh_sw + (size_t)ox * q;
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

static void prepare_palette(void)
{
    if (!luts_ready) init_luts();
    for (int i = 0; i < 16; i++) {
        u32 c = gfx_palette_rgb((u8)i);
        pal_lin[i][0] = lin_of[c >> 16 & 255];
        pal_lin[i][1] = lin_of[c >> 8 & 255];
        pal_lin[i][2] = lin_of[c & 255];
    }
    pal_key_used = enh_palette_key();
}

/* ------------------------------------------------------------------------------------------------ */

static void render_band(int i, void *ctx)
{
    (void)ctx;
    Band b = { band_o0[i] * enh_q, band_o0[i + 1] * enh_q, i };
    memset(smp + (size_t)b.r0 * enh_sw, 0, (size_t)(b.r1 - b.r0) * enh_sw);
    for (int r = b.r0; r < b.r1; r++) g_near[r] = g_far[r] = -1;
    const EnhScene *S = &enh_sc;
    for (int k = 0; k < S->ncmds; k++) {
        const EnhCmd *c = &S->cmds[k];
        switch (c->type) {
        case CMD_FILL:   do_fill(&b, c); break;
        case CMD_SPRITE: do_sprite(&b, c); break;
        case CMD_LINE:   do_line(&b, c); break;
        case CMD_GROUND: do_ground(&b); break;
        case CMD_WALLS:  do_walls(&b, c); break;
        case CMD_BAND:   do_band(&b, c); break;
        case CMD_MARK:   do_mark(&b, c); break;
        default: break;
        }
    }
    resolve_rows(band_o0[i], band_o0[i + 1]);
}

void enh_raster_render(void)
{
    prepare_palette();
    host_parallel_for(nbands, render_band, NULL);
}

static void resolve_band(int i, void *ctx)
{
    (void)ctx;
    resolve_rows(band_o0[i], band_o0[i + 1]);
}

void enh_resolve(void)
{
    prepare_palette();
    host_parallel_for(nbands, resolve_band, NULL);
}
