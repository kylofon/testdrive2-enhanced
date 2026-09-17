/* Enhanced renderer: the front view in continuous depth (ENHANCED.md "Smooth motion", "Projection",
 * "Draw distance").
 *
 * This is the faithful front-view code (game/scene_project.c, scene_draw.c, scene_objects.c) evaluated in
 * floating point for rows at continuous depths, with more rows, producing a display list in original
 * buffer coordinates instead of drawing. The structure, the variables and the drawing order follow the
 * original so that every feature (drop-offs, cliffs, tunnels, bands, objects, cars) behaves the same. */
#include "enh_internal.h"
#include "../game/scene.h"
#include "../platform/res.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

EnhScene enh_sc;

#define ROAD0    0x3B51                  /* first road byte */
#define DAT_END  0x52C8                  /* end of the stage image */
#define KX       (195256.0 / 65536.0)    /* xs_front[i] = 195256 / (i + 4) */
#define KY       (140672.0 / 65536.0)    /* ys_front[i] = 140672 / (i + 4) */
#define KW       1200.0                  /* w_front[i]  = 1200 / (i + 4) */
#define FADE     0.1                     /* objects fade in over the last 10 % of their distance */
#define CUT_ROWS 60                      /* rows that place the cliff and drop-off cut lines (the original's) */

static EnhScene *S = &enh_sc;

/* ------------------------------------------------------------------------------------------------ */
/* road data                                                                                        */

static u8 road_byte(int u)
{
    long a = (long)ROAD0 + u;
    if (a < ROAD0 || a >= DAT_END) return 0;
    return DSB((u16)a);
}

static const u8 *road_rec(int u) { return mp(DGROUP, (u16)(DS_road_records + (road_byte(u) & 0x7F) * 4)); }

/* tan256 of an 8.8 degree accumulator. The original indexes the table by whole degrees (tan_deg8 of the
 * high byte); here the table is interpolated within the degree, so that bends and hills, which the
 * accumulators follow in fractions of a degree per unit, turn the view evenly instead of in whole-degree
 * steps (equal to the original at whole degrees). */
static double tan8(double acc)
{
    double d = acc / 256.0, fl = floor(d);
    int k = (int)fl;
    if (k < -127) k = -127;
    if (k > 126) k = 126;
    double a = tan_deg8((u8)(s8)k), b = tan_deg8((u8)(s8)(k + 1));
    return a + (b - a) * (d - fl);
}

/* The original's integrators (project_rows) for a view whose car is at unit `org` and whose heading
 * starts at yaw0: H (height sum) and X (lateral sum without the car's lateral) of units org - 1 ..
 * org - 1 + n - 1. Unit org - 1 is one unit behind the car, on the car's own slope and heading. */
static void integrate(int org, double yaw0, int n, double *H, double *X)
{
    double pacc = 0, hacc = yaw0, h = 80, x = 0;          /* DS:09B0 start height */
    H[0] = 80;
    X[0] = -tan8(yaw0);
    H[1] = 80;
    X[1] = 0;
    for (int k = 2; k < n; k++) {
        const u8 *r = road_rec(org - 1 + k);
        pacc += (s16)((s16)-(s8)r[2] >> 1);
        h += tan8(pacc);
        hacc += (s8)r[1] * 16;
        if (hacc < -0x4600) hacc = -0x4600;
        else if (hacc > 0x4600) hacc = 0x4600;
        x += tan8(hacc);
        H[k] = h;
        X[k] = x;
    }
}

/* lane widening of unit u (scene_project.c widen): distance from the last change of the wide bit */
static bool widen(int u, float W, float *ext)
{
    bool wide = road_byte(u) & 0x80;
    int d = 8;
    for (int k = 0; k < 8; k++)
        if (((road_byte(u - k) ^ road_byte(u - k - 1)) & 0x80) != 0) { d = k; break; }
    if (d < 8) {
        *ext = W * (float)(wide ? d : 8 - d) / 8.0f;
        return true;
    }
    if (wide) { *ext = W; return true; }
    return false;
}

/* far-right ground band (DAT+0x33C) of unit u: x offset from the outer right edge, or -1 (none) */
static float band_offset(int u, float W)
{
    u16 dx = (u16)(u + 31);                              /* walk_ptr - 0x3B33 */
    u16 b = 0, bpe = 0;
    bool found = false;
    while (DSW((u16)(DS_right_zones + b)) != 0) {
        bpe = DSW((u16)(DS_right_zones + 2 + b));
        if (dx <= bpe) { found = true; break; }
        b = (u16)(b + 8);
    }
    u16 start = DSW((u16)(DS_right_zones + b));
    if (!found || dx < start || bpe == start) return -1;
    double v0 = DSS((u16)(DS_right_zones + 4 + b)), v1 = DSS((u16)(DS_right_zones + 6 + b));
    double v = v0 + (v1 - v0) * (double)(u16)(dx - start) / (double)(u16)(bpe - start);
    return (float)(v * W / 256.0);
}

/* ------------------------------------------------------------------------------------------------ */
/* display list                                                                                     */

static float cur_cy0, cur_cy1, cur_cx0, cur_cx1 = VIEW_W, cur_alpha = 1;

static EnhCmd *cmd(u8 type)
{
    if (S->ncmds == S->cap) {
        int nc = S->cap ? S->cap * 2 : 4096;
        EnhCmd *p = realloc(S->cmds, (size_t)nc * sizeof *p);
        if (!p) return NULL;
        S->cmds = p;
        S->cap = nc;
    }
    EnhCmd *c = &S->cmds[S->ncmds++];
    memset(c, 0, sizeof *c);
    c->type = type;
    c->cy0 = cur_cy0;
    c->cy1 = cur_cy1;
    c->cx0 = cur_cx0;
    c->cx1 = cur_cx1;
    c->alpha = cur_alpha;
    return c;
}

static void fill(float x, float y, float w, float h, u16 colour)          /* 06c9:89a2 */
{
    if (!(w > 0) || !(h > 0) || cur_alpha <= 0) return;
    EnhCmd *c = cmd(CMD_FILL);
    if (!c) return;
    c->x0 = x; c->y0 = y; c->x1 = x + w; c->y1 = y + h;
    c->colour = (u8)(colour & 15);
}

static float line_w = 1;

/* 06c9:8582 draw_line between pixel centres, both ends included */
static void line(float x0, float y0, float x1, float y1, u16 colour)
{
    if (cur_alpha <= 0) return;
    EnhCmd *c = cmd(CMD_LINE);
    if (!c) return;
    c->x0 = x0 + 0.5f; c->y0 = y0 + 0.5f; c->x1 = x1 + 0.5f; c->y1 = y1 + 0.5f;
    c->w = line_w;
    c->colour = (u8)(colour & 15);
}

/* sprite with its hot spot at (x, y), drawn k times its size; the hot-spot pixel stays centred */
static void blit(FarPtr h, int op, float x, float y, float k)
{
    if (cur_alpha <= 0 || !(k > 0)) return;
    const EnhSprite *s = enh_sprite(h);
    if (!s) return;
    EnhCmd *c = cmd(CMD_SPRITE);
    if (!c) return;
    c->spr = s;
    c->op = (u8)op;
    c->x0 = x + 0.5f - ((float)s->hx + 0.5f) * k;
    c->y0 = y + 0.5f - ((float)s->hy + 0.5f) * k;
    c->x1 = k;
}

static FarPtr hnd_at(u16 ds_off) { return ds_far(ds_off); }
static void and_h(u16 h, float x, float y, float k) { blit(hnd_at(h), EOP_AND, x, y, k); }
static void or_h(u16 h, float x, float y, float k)  { blit(hnd_at(h), EOP_OR, x, y, k); }
static void xor_h(u16 h, float x, float y, float k) { blit(hnd_at(h), EOP_XOR, x, y, k); }
static void copy_h(u16 h, float x, float y, float k) { blit(hnd_at(h), EOP_COPY, x, y, k); }

/* Sprite sizes. The original draws size variant k of a group (4 sizes by scale4, 5 by scale5, 8 cars by
 * carscale) unscaled on the rows that select it. Here the group has one continuous height S(W), linear
 * between the variants' heights at the centre of their original depth ranges and proportional to W
 * outside the original's 60 rows; the chosen variant is scaled to that height. */
typedef struct { int n; double wnom[8]; bool ok[8]; } Family;
static Family fam4, fam5, famcar;

static int variant4(int i) { int os = (1200 / (i + 4)) >> 3; if (os > 31) os = 31; return (os >> 1) / 4; }
static int variant5(int i) { int os = (1200 / (i + 4)) >> 3; if (os > 31) os = 31; return (os >= 16 ? 16 : os) / 4; }
static int variantcar(int i) { return DSB((u16)(DS_carscale_front + 2 * i)) & 7; }

static void family_init(Family *f, int n, int (*variant)(int))
{
    f->n = n;
    for (int k = 0; k < n; k++) {
        int lo = 99, hi = -1;
        for (int i = 0; i < 60; i++)
            if (variant(i) == k) { if (i < lo) lo = i; if (i > hi) hi = i; }
        f->ok[k] = hi >= 0;
        f->wnom[k] = hi >= 0 ? KW / sqrt((lo + 4.0) * (hi + 5.0)) : 0;
    }
}

/* scale for variant k of the group whose variant q is the handle at base + stride * q */
static float group_scale(u16 base, int stride, int k, const Family *f, double W)
{
    const EnhSprite *sk = enh_sprite(hnd_at((u16)(base + stride * k)));
    if (!sk) return 1;
    double pw[8], ph[8];
    int m = 0;
    for (int q = 0; q < f->n; q++) {
        if (!f->ok[q]) continue;
        const EnhSprite *sq = enh_sprite(hnd_at((u16)(base + stride * q)));
        if (!sq) continue;
        pw[m] = f->wnom[q];
        ph[m] = sq->h;
        m++;
    }
    if (m == 0) return 1;
    double h;
    if (W <= pw[0]) h = ph[0] * W / pw[0];
    else if (W >= pw[m - 1]) h = ph[m - 1] * W / pw[m - 1];
    else {
        int a = 0;
        while (a + 2 < m && W >= pw[a + 1]) a++;
        h = ph[a] + (ph[a + 1] - ph[a]) * (W - pw[a]) / (pw[a + 1] - pw[a]);
    }
    return (float)(h / sk->h);
}

/* A group whose variants all have about the same height while their width grows (the CCC redwood trunks)
 * is cut off by the top of the view wherever the original draws it; scaled down it would be a column of
 * its own. Such groups are drawn only within the original's scenery distance. */
static bool group_cut_off(u16 base, const Family *f)
{
    const EnhSprite *lo = NULL, *hi = NULL;
    double wlo = 0, whi = 0;
    for (int q = 0; q < f->n; q++) {
        const EnhSprite *sq = f->ok[q] ? enh_sprite(hnd_at((u16)(base + 4 * q))) : NULL;
        if (!sq) continue;
        if (!lo) { lo = sq; wlo = f->wnom[q]; }
        hi = sq;
        whi = f->wnom[q];
    }
    if (!lo || lo == hi) return false;
    return (double)hi->h / lo->h < 0.5 * whi / wlo;
}

#define SCALE4(base, W) group_scale((base), 4, s4_cur, &fam4, (W))
#define SCALE5(base, W) group_scale((base), 4, s5_cur, &fam5, (W))

static float fade(double z, double zlim)
{
    double a = (zlim - z) / (FADE * zlim);
    return a >= 1 ? 1.0f : a <= 0 ? 0.0f : (float)a;
}

/* ------------------------------------------------------------------------------------------------ */
/* rows (project_rows)                                                                              */

static int nrows;
static double frac;           /* car progress through its unit */
static int car_unit;          /* V */
static int step_unit;         /* unit of the last simulation step */
static double H_a[ENH_MAX_ROWS + 4], H_b[ENH_MAX_ROWS + 4];
static double X_aa[ENH_MAX_ROWS + 4], X_ba[ENH_MAX_ROWS + 4];

static void project(const EnhView *v)
{
    nrows = enh_rows_setting;
    S->nrows = nrows;
    car_unit = (int)floor(v->s);
    frac = v->s - car_unit;
    step_unit = (int)v->pos - ROAD0;

    /* views at the car's unit (a) and the next one (b), each with its view_yaw and lateral, blended by
     * the sub-unit fraction */
    int n = nrows + 3;
    integrate(car_unit, v->yaw_a, n, H_a, X_aa);
    integrate(car_unit + 1, v->yaw_b, n, H_b, X_ba);

    /* region state at the car's unit (the simulation's DS:5491 moved on to it) */
    u8 sf = v->start_flags;
    for (int u = step_unit + 1; u <= car_unit; u++) {
        u8 b = road_byte(u);
        sf = (u8)(((sf & 0xFE) | (b >> 7)) ^ road_rec(u)[0]);
    }
    S->start_flags = sf;
    S->median = DSB(DS_median) != 0;
    S->style = DSB(DS_toggle_37f7) != 0;
    S->backdrop_off = DSB(DS_backdrop_off) != 0;

    for (int j = 0; j <= nrows + 1; j++) {
        EnhRow *r = &S->rows[j];
        int u = car_unit + j;
        double f = frac;
        r->unit = u;
        r->z = j + 3 - f;
        r->H = (1 - f) * H_a[j + 1] + f * H_b[j];
        r->X = (1 - f) * (X_aa[j + 1] - v->lat_a) + f * (X_ba[j] - v->lat_b);
        double z = r->z;
        r->cx = (float)(125.0 + r->X * KX / z);
        r->y = (float)(51.0 + r->H * KY / z);
        float W = (float)(KW / z);
        r->W = W;
        float ext;
        float L = r->cx - W;
        if (S->median && widen(u, W, &ext)) L -= ext;
        float R = r->cx + W;
        if (widen(u, W, &ext)) R += ext;
        r->L = L;
        r->R = R;
        r->ol = L - W / 4;
        r->or_ = R + W / 4;
        float bo = band_offset(u, W);
        r->band = bo < 0 ? 2560.0f : r->or_ + bo;
        u8 b = road_byte(u);
        const u8 *rc = road_rec(u);
        r->flags = (u8)((b >> 7) | rc[0]);
        r->obj = rc[3];
        r->phase = (u8)(v->counter + (u - step_unit - 1));
    }
}

/* the original's projection state machine over rows 1..nrows (row 1 = the original's row 0) */
static void cut_lines(void)
{
    u8 st = S->start_flags;
    S->r0_any = st;
    S->left_cut_state = S->right_cut_state = st;
    S->top_sy = VIEW_H;
    S->top_row = 1;
    S->left_cut_x = 0;
    S->right_cut_x = VIEW_W;
    S->left_sky_x = VIEW_W;
    S->right_sky_x = 0;
    S->left_sky_y = S->right_sky_y = 0;
    S->left_cut_row = S->right_cut_row = S->left_sky_row = S->right_sky_row = 1;
    S->tunnel_in_row = S->tunnel_out_row = S->tunnel_ceiling_row = 1;
    S->tunnel_in_sy = VIEW_H;
    S->tunnel_in_top = S->tunnel_out_sy = S->tunnel_out_top = S->tunnel_ceiling = 0;
    S->tunnel_in_l = 0;
    S->tunnel_in_r = VIEW_W;
    S->tunnel_out_l = S->tunnel_out_r = 0;
    S->nextra = 0;
    bool out_found = false;
    /* The original's per-frame state describes its 60 rows: the cut lines of cliffs and drop-offs, which
     * reach the top of the view, and the tunnel whose portal fills everything above it. Here those come
     * from the same rows (CUT_ROWS); only the far end of that tunnel is looked for at any distance.
     * Tunnels beyond it are kept in extra[]; a non-style tunnel that starts farther away shows its ribs
     * until it comes within the original's distance, where the original's portal takes over. */
    int tstage = (st & 0x80) ? 1 : 0;                      /* nearest tunnel: none / open / closed */
    int open_extra = -1;

    S->rows[0].state = st;
    S->rows[0].clip = VIEW_H;
    for (int j = 1; j <= nrows; j++) {
        EnhRow *r = &S->rows[j];
        u8 r0 = road_rec(r->unit)[0];
        st ^= r0;
        r->state = st;
        bool near = j <= CUT_ROWS;
        if (r->y < S->top_sy) {
            S->top_sy = r->y;
            S->top_row = j;
        }
        r->clip = S->top_sy;

        float ys = r->y < VIEW_H ? r->y : VIEW_H;
        float tp = r->y - r->W / 2;
        if (!(tp > 0)) tp = 0;
        bool handled = false;
        if (r0 & 0x80) {
            if (!(st & 0x80)) {                            /* far end */
                if (tstage == 1) {
                    S->tunnel_out_row = j;
                    S->tunnel_out_sy = ys;
                    S->tunnel_out_top = tp;
                    out_found = true;
                    tstage = 2;
                    S->sky_state = st;
                    handled = true;
                } else if (open_extra >= 0) {
                    EnhTunnel *t = &S->extra[open_extra];
                    t->out_row = j;
                    t->out_sy = ys;
                    t->out_top = tp;
                    t->has_out = true;
                    open_extra = -1;
                    handled = true;
                }
            } else if (tstage == 0 && near) {              /* entrance */
                S->tunnel_in_row = j;
                S->tunnel_in_sy = ys;
                S->tunnel_in_top = tp;
                tstage = 1;
                handled = true;
            } else if ((tstage == 2 || (tstage == 0 && S->style)) && S->nextra < ENH_MAX_TUNNELS) {
                open_extra = S->nextra++;
                EnhTunnel *t = &S->extra[open_extra];
                memset(t, 0, sizeof *t);
                t->in_row = j;
                t->in_sy = ys;
                t->in_top = tp;
                handled = true;
            }
        }
        bool in_nearest = tstage == 1 && (st & 0x80);
        if (in_nearest && tp >= S->tunnel_ceiling) {
            S->tunnel_ceiling = tp;
            S->tunnel_ceiling_row = j;
        }
        if (near || in_nearest) S->r0_any |= r0;
        else if (handled) S->r0_any |= 0x80;

        bool hit = false;
        if ((st & 0xC0) && (near || in_nearest)) {
            float a = (st & 0x80) ? r->L : r->ol;
            if (a > S->left_cut_x) {
                S->left_cut_x = a;
                S->left_cut_row = j;
                S->left_cut_state = st;
                hit = true;
            }
        }
        if (!hit && near && j != 1 && r->ol < S->rows[j - 1].ol && r->y <= S->top_sy && r->ol < S->left_sky_x) {
            S->left_sky_x = r->ol;
            S->left_sky_row = j;
        }
        hit = false;
        if ((st & 0x88) && (near || in_nearest)) {
            float a = (st & 0x80) ? r->R : r->or_;
            if (a < S->right_cut_x) {
                S->right_cut_x = a;
                S->right_cut_row = j;
                S->right_cut_state = st;
                hit = true;
            }
        }
        if (!hit && near && j != 1 && r->or_ > S->rows[j - 1].or_ && r->y <= S->top_sy && r->or_ > S->right_sky_x) {
            S->right_sky_x = r->or_;
            S->right_sky_row = j;
        }
    }
    S->rows[1].clip = VIEW_H;
    S->r0_state = st;
    if (tstage != 2) S->sky_state = st;
    S->tunnel_out_found = out_found;
    S->tunnel_nearest = tstage != 0;
    for (int e = 0; e < S->nextra; e++) {
        EnhTunnel *t = &S->extra[e];
        if (!t->has_out) {
            t->out_row = nrows;
            t->out_sy = t->out_top = S->top_sy;
        }
    }

    /* fix_cut_lines */
    u8 bl = S->r0_any;
    if (bl == 0) return;
    if (bl & 0x20) {
        S->left_sky_y = S->rows[S->left_sky_row].y;
        S->left_sky_x = S->left_sky_x < 0 ? 0 : S->left_sky_x > VIEW_W ? VIEW_W : S->left_sky_x;
    }
    if (bl & 0x04) {
        S->right_sky_y = S->rows[S->right_sky_row].y;
        S->right_sky_x = S->right_sky_x < 0 ? 0 : S->right_sky_x > VIEW_W ? VIEW_W : S->right_sky_x;
    }
    float d = S->left_cut_x;
    if ((bl & 0x40) && !(S->left_cut_state & 0x80)) d -= 22;
    S->left_cut_x = d <= 0 ? 0 : d < VIEW_W ? d : VIEW_W;
    d = S->right_cut_x;
    if ((bl & 0x08) && !(S->right_cut_state & 0x80)) d += 22;
    S->right_cut_x = d <= 0 ? 0 : d < VIEW_W ? d : VIEW_W;
    if (!(bl & 0x80)) return;
    if (!out_found && S->tunnel_nearest) {
        S->tunnel_out_sy = S->tunnel_out_top = S->top_sy;
        S->tunnel_out_row = nrows;
    }
    if (S->tunnel_ceiling_row < S->top_row && S->tunnel_ceiling > S->top_sy) S->top_sy = S->tunnel_ceiling;
    if (S->left_cut_row != S->right_cut_row) {
        if (S->left_cut_row < S->right_cut_row) {
            if (S->right_cut_x <= S->left_cut_x) S->right_cut_x = S->left_cut_x;
        } else {
            if (S->left_cut_x >= S->right_cut_x) S->left_cut_x = S->right_cut_x;
        }
    }
}

/* clamp_edge (06c9:08cb) */
static float edge_clamp(float x, int j)
{
    u8 f = S->r0_any;
    if (f & 0x88) {
        if (x < 0) x = 0;
        else if (j > S->right_cut_row) { if (x > S->right_cut_x) x = S->right_cut_x; }
        else if (x > VIEW_W) x = VIEW_W;
    }
    if (f & 0xC0) {
        if (x > VIEW_W) return VIEW_W;
        if (j < S->left_cut_row) return x;
        if (x < S->left_cut_x) return S->left_cut_x;
    }
    return x;
}

/* ground ownership: rows are taken near to far, each scanline belongs to the first pair covering it */
static void ground_pairs(void)
{
    if ((S->r0_any & 0x80)) {
        S->tunnel_in_l = edge_clamp(S->rows[S->tunnel_in_row].L, S->tunnel_in_row);
        S->tunnel_in_r = edge_clamp(S->rows[S->tunnel_in_row].R, S->tunnel_in_row);
        S->tunnel_out_l = edge_clamp(S->rows[S->tunnel_out_row].L, S->tunnel_out_row);
        S->tunnel_out_r = edge_clamp(S->rows[S->tunnel_out_row].R, S->tunnel_out_row);
        for (int e = 0; e < S->nextra; e++) {
            EnhTunnel *t = &S->extra[e];
            t->in_l = edge_clamp(S->rows[t->in_row].L, t->in_row);
            t->in_r = edge_clamp(S->rows[t->in_row].R, t->in_row);
            t->out_l = edge_clamp(S->rows[t->out_row].L, t->out_row);
            t->out_r = edge_clamp(S->rows[t->out_row].R, t->out_row);
        }
    }
    S->npairs = 0;
    float clip = VIEW_H + 64;                              /* below the window: the nearest pair */
    int bp = 0;
    for (int j = 1; j <= nrows; j++) {
        float y = S->rows[j].y;
        if (y == S->rows[bp].y) continue;
        if (y < clip) {
            EnhPair *p = &S->pairs[S->npairs++];
            p->ylo = y;
            p->yhi = clip;
            p->near = bp;
            p->far = j;
            clip = y;
        }
        bp = j;                                            /* build_spans: the previous row with another y */
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* sky (draw_sky)                                                                                    */

static void sky(const EnhView *v)
{
    float top = S->top_sy;
    u16 skyc = S->col_sky;
    cur_cy0 = 0;
    cur_cy1 = VIEW_H;
    cur_alpha = 1;
    if (!S->style) {
        u8 bl = S->r0_any;
        if ((bl & 0x80) && S->tunnel_nearest) {
            float di = S->tunnel_out_l, bp = S->tunnel_out_r;
            if (S->sky_state & 0x80) {
                fill(di, S->tunnel_in_top, bp - di, S->tunnel_out_top - S->tunnel_in_top, 0);
                return;
            }
            float tt = S->tunnel_in_top, ceil = S->tunnel_ceiling;
            if (bl & 0x40) {
                float lc = S->left_cut_x;
                fill(lc, ceil, bp - lc, top - ceil, skyc);
            } else {
                fill(di, ceil, S->right_cut_x - di, top - ceil, skyc);
            }
            fill(di, tt, bp - di, ceil - tt, 0);
            return;
        }
        if (bl & 0x40) {
            float lc = S->left_cut_x;
            if (bl & 0x08) fill(lc, 0, S->right_cut_x - lc, top, skyc);
            else fill(lc, 0, VIEW_W - lc, top, skyc);
            return;
        }
        if (bl & 0x08) {
            fill(0, 0, S->right_cut_x, top, skyc);
            return;
        }
    }
    fill(0, 0, VIEW_W, top, skyc);
    if (S->backdrop_off) return;
    u16 hb = DS_scenery_sky_handles;
    if (DSW((u16)(hb + 8 * 4 + 2)) == 0) return;          /* mtn0 */
    double hy = v->yaw * 8.0 / 256.0;                      /* (s8)((view_yaw << 3) >> 8) */
    double di = fmod(-(v->heading - hy), 1024.0);
    if (di < 0) di += 1024.0;
    copy_h((u16)(hb + 8 * 4), (float)(di - 1024), top, 1);
    copy_h((u16)(hb + 10 * 4), (float)(di - 500), top, 1);
    copy_h((u16)(hb + 9 * 4), (float)(di - 300), top, 1);
    copy_h((u16)(hb + 8 * 4), (float)di, top, 1);
    if (DSW((u16)(hb + 11 * 4 + 2)) == 0) return;         /* clo1 */
    di = fmod(-(v->cloud - hy), 1024.0);
    if (di < 0) di += 1024.0;
    /* y of the original's farthest row (depth 63) */
    double jf = 60.0 + frac;
    int j0 = (int)jf;
    float cy = S->rows[j0].y + (S->rows[j0 + 1].y - S->rows[j0].y) * (float)(jf - j0) - 30;
    di -= 100;
    copy_h((u16)(hb + 11 * 4), (float)(di - 1024), cy, 1);
    copy_h((u16)(hb + 13 * 4), (float)(di - 500), cy, 1);
    copy_h((u16)(hb + 12 * 4), (float)(di - 300), cy, 1);
    copy_h((u16)(hb + 11 * 4), (float)di, cy, 1);
}

/* ------------------------------------------------------------------------------------------------ */
/* objects (draw_objects)                                                                           */

static int j_cur;
static u8 st_cur;
static float W_cur, os_cur;
static int s4_cur, s5_cur;

static void set_ceiling_clip(int j)
{
    if (!S->style) cur_cy0 = S->tunnel_ceiling;
    if ((S->r0_any & 0x80) && j <= S->tunnel_ceiling_row) cur_cy0 = 0;
}

static void rib(int j)                                              /* 06c9:1ad6 */
{
    const EnhRow *r = &S->rows[j];
    float bx = r->y, cx = r->y - r->W / 2;
    if (S->style) cx = S->top_sy - 5;
    if (!S->style) line(r->L, cx, r->R, cx, 15);
    line(r->R, cx, r->R, bx, 15);
    line(r->L, cx, r->L, bx, 15);
}

static void cliff_wall(int j, bool left)
{
    const EnhRow *r = &S->rows[j];
    float ax = left ? S->left_cut_x : S->right_cut_x;
    float cx = r->clip, di, bp;
    if ((S->r0_any & 0x80) && j >= S->tunnel_out_row) {
        di = S->tunnel_ceiling;
        cx -= di;
        bp = left ? S->tunnel_out_l : S->tunnel_out_r;
    } else {
        di = 0;
        bp = left ? 0 : VIEW_W;
    }
    if (left) fill(bp, di, ax - bp, cx, 6);
    else fill(ax, di, bp - ax, cx, 6);
    float x = left ? r->ol : r->or_;
    float dy = r->y < VIEW_H ? r->y : VIEW_H;
    u16 hb = DS_scenery_sky_handles;
    u16 k = left ? 4 : 0;                                  /* lcfA / lcfa, rcfA / rcfa */
    and_h((u16)(hb + 4 * k), x, dy, 1);
    or_h((u16)(hb + 4 * (k + 1)), x, dy, 1);
}

/* a cliff row beyond the cut-line rows: a wall of limited height beside the road */
/* a cliff row beyond the cut-line rows: a wall segment of limited height (W pixels, about 560 height
 * units) along the outer edge between this row and the nearer one, reaching W outwards */
static void far_cliff(int j, bool left)
{
    const EnhRow *r = &S->rows[j], *n = &S->rows[j - 1];
    float top = r->y - r->W;
    float a = left ? r->ol : r->or_, b = left ? n->ol : n->or_;
    float x0 = a < b ? a : b, x1 = a < b ? b : a;
    if (left) x0 -= r->W;
    else x1 += r->W;
    fill(x0, top, x1 - x0, r->y - top, 6);
}

static void cliff_deco(int j, bool left)
{
    const EnhRow *r = &S->rows[j];
    u16 w = DSW((u16)(DS_cliff_deco_pattern + (((u8)(r->phase << 1)) & 0x1E)));
    if ((u8)w == 0) return;
    float x = left ? r->ol : r->or_;
    if (!(x >= 0 && x < VIEW_W)) return;
    u8 cl = (u8)(w >> 8);
    u16 base = (u16)(SCN_H(left ? 184 : 160) + (u16)((u16)((u8)w - 1) << 4));
    float k = SCALE4((u16)(base + 0x30), W_cur);
    float dy = r->y;
    if (cl != 0) dy -= os_cur / 2 * cl;
    and_h((u16)(base + 0x30 + 4 * s4_cur), x, dy, k);      /* upper case = mask */
    or_h((u16)(base + 4 * s4_cur), x, dy, k);
}

static void tunnel_mouths(int j, const EnhTunnel *T, bool nearest)        /* §4.8.1 */
{
    const EnhRow *r = &S->rows[j];
    bool style = S->style;
    float wd = VIEW_W;
    u16 hb = DS_scenery_sky_handles;
    u16 skyc = S->col_sky;
    if (j == T->out_row) {                                 /* far end */
        u16 dx = 0;
        float bx = T->in_top, cx = T->out_sy;
        float ax = T->in_l, di = T->out_l;
        if (style) {
            dx = 8;
            di = r->L;
            bx = S->top_sy - 5;
        }
        if (di <= ax) { float t = di; di = ax; ax = t; }
        cx -= bx;
        di -= ax;
        float ax2 = T->out_r, di2 = T->in_r;
        if (style) {
            ax2 = r->R;
            if (di2 <= ax2) { float t = di2; di2 = ax2; ax2 = t; }
        }
        di2 -= ax2;
        fill(ax2, bx, di2, cx, dx);
        fill(ax, bx, di, cx, dx);
        if (style) rib(j);
    } else if (nearest && !style && (r->state & 0x80)) {
        if (j == S->right_cut_row) {
            float bx = S->tunnel_in_top, ax = S->right_cut_x;
            fill(ax, bx, S->tunnel_in_r - ax, r->clip - bx, 0);
        }
        if (j == S->left_cut_row) {
            float bx = S->tunnel_in_top, dx = S->tunnel_in_l;
            fill(dx, bx, S->left_cut_x - dx, r->clip - bx, 0);
        }
    }
    if (j != T->in_row) return;                            /* near end (entrance) */
    float clip = r->clip, in_l = T->in_l, in_r = T->in_r, in_top = T->in_top;
    if (nearest && (S->start_flags & 0x80)) {              /* car already inside */
        if (!style) {
            fill(0, 0, in_l, clip, 0);
            fill(in_r, 0, wd - in_r, clip, 0);
        } else {
            float t = S->top_sy - 5, h = clip - t;
            fill(in_r, t, wd - in_r, h, 8);
            fill(0, t, in_l, h, 8);
        }
        return;
    }
    if (style) {
        rib(j);
        return;
    }
    float top_sy = S->top_sy;
    if (!(r->state & 0x40)) {                              /* portal A */
        float bp = in_l, di = S->left_sky_x, px;
        bool first;
        if (j > S->left_sky_row) first = true;
        else if (di == 0) first = false;
        else { di += 15; first = bp <= di; }
        if (first) {
            fill(0, 0, bp, top_sy, skyc);
            px = r->L;
        } else {
            fill(di, 0, bp - di, clip, 6);
            bp = di;
            if (bp == 0) goto walls_a;
            fill(0, 0, bp, top_sy, skyc);
            px = bp;
        }
        and_h((u16)(hb + 2 * 4), px, 0x5B, 1);             /* rcfB */
        or_h((u16)(hb + 3 * 4), px, 0x5B, 1);              /* rcfb */
    walls_a:
        fill(bp, 0, wd - bp, in_top, 6);
        fill(in_r, in_top, wd - in_r, clip - in_top, 6);
    } else {                                               /* portal B */
        float bp = in_r, di = S->right_sky_x, px;
        bool first;
        if (j > S->right_sky_row) first = true;
        else if (di == wd) first = false;
        else { di -= 15; first = bp >= di; }
        if (first) {
            fill(bp, 0, wd - bp, top_sy, skyc);
            px = r->R;
        } else {
            fill(in_r, 0, di - in_r, clip, 6);
            bp = di;
            fill(bp, 0, wd - bp, top_sy, skyc);
            px = bp;
        }
        and_h((u16)(hb + 6 * 4), px, 0x5B, 1);             /* lcfB */
        or_h((u16)(hb + 7 * 4), px, 0x5B, 1);              /* lcfb */
        fill(0, 0, bp, in_top, 6);
        fill(0, in_top, in_l, clip - in_top, 6);
    }
    if (j > 1) rib(j);                              /* the original skips its row 0 */
}

static void road_object(int j, u8 o)                                       /* §4.9 */
{
    const EnhRow *r = &S->rows[j];
    float y = r->y, W = r->W, R = r->R, L = r->L;
    if (o >= 1 && o <= 9) {                                /* signs on both sides */
        u16 base = (u16)(ROAD_H(ROAD_sa) + ((o - 1) << 4));
        float k = SCALE4(base, W);
        u16 di = (u16)(base + 4 * s4_cur);
        u16 bx = (u16)(ROAD_H(ROAD_pst0) + 4 * s4_cur);
        float dy = y - os_cur;
        or_h(bx, R, dy, k);
        or_h(bx, L, dy, k);
        and_h(di, R, dy, k);
        and_h(di, L, dy, k);
        or_h((u16)(di + 0x90), R, dy, k);
        or_h((u16)(di + 0x90), L, dy, k);
    } else if (o == 10 || o == 12) {                       /* white band across the road */
        float save = cur_cy1;
        cur_cy1 = VIEW_H;
        float y1 = j - 3 >= 1 ? S->rows[j - 3].y : VIEW_H;
        float y0 = y < VIEW_H ? y : VIEW_H;
        float min_h = 1.0f / (float)enh_scale;
        if (y1 < y0 + min_h) y1 = y0 + min_h;
        EnhCmd *c = cmd(CMD_BAND);
        if (c) {
            c->y0 = y0;
            c->y1 = y1;
            c->colour = 15;
        }
        cur_cy1 = save;
    } else if (o == 11) {                                  /* end of stage */
        if (DSW(DS_last_stage) == 0) {                     /* gas station sign */
            u16 base = ROAD_H(ROAD_GST0);
            float k = SCALE5(base, W);
            u16 di = (u16)(base + 4 * s5_cur);
            float dx = W + R;
            and_h(di, dx, y, k);
            or_h((u16)(di + 0x14), dx, y, k);
        } else {                                           /* FINISH banner */
            float h = W / 2 + W / 4, p = W / 8;
            float top = y - h, bxp = L;
            fill(bxp, top, R - bxp, 2 * p, 15);
            fill(R, top, p, h, 15);
            fill(L - p, top, p, h, 15);
            float ox = bxp + p;
            float s = (float)(KX / r->z);
            for (u16 q = DS_finish_letters;;) {
                u16 w0 = DSW(q); q = (u16)(q + 2);
                if ((s16)w0 < 0) break;
                u16 w1 = DSW(q); q = (u16)(q + 2);
                u16 w2 = DSW(q); q = (u16)(q + 2);
                u16 w3 = DSW(q); q = (u16)(q + 2);
                line(w2 * s + ox, w3 * s + top, w0 * s + ox, w1 * s + top, 0);
            }
        }
    } else if (o >= 13 && o <= 20) {                       /* hazards (XOR) */
        u8 al = (u8)(o - 10);
        float cx = W / 2;
        u16 ax = (u16)(al << 1);
        if (ax & 2) cx = -cx;
        cx += r->cx;
        ax = (u8)(((u8)ax - 6) & 0xFC);
        u16 base = (u16)(ROAD_H(ROAD_rck) + (ax << 2));
        float k = SCALE4(base, W);
        xor_h((u16)(base + 4 * s4_cur), cx, y, k);
    }
}

static void text_sign(int j, u16 k)                                        /* §4.10 SGN signs */
{
    const EnhRow *r = &S->rows[j];
    s8 t = DSC((u16)(DS_dat_scenery_type + k));
    u16 sgn = DSW(DS_sgn_segment), fnt = DSW(DS_fnt_segment);
    if (sgn == 0 || fnt == 0) return;
    u16 q = (u16)((u8)((u8)t - 0x1E) / 5);
    u16 ofs = rd16(sgn, (u16)(q << 1));
    if (ofs == 0) return;
    u16 rec[0x2D];
    for (u16 n = 0; n < 0x2D; n++) rec[n] = rd16(sgn, (u16)(ofs + 2 * n));
    double W = r->W;
    float sw = (float)(rec[0] * W / 512.0), sh = (float)(rec[1] * W / 512.0);
    float post_h = (float)(rec[12] * W / 512.0), pw = (float)(W / 16.0);
    float x = (float)((s8)DSB((u16)(DS_dat_scenery_offset + k)) * W / 8.0);
    x += (x > 0 ? r->R : r->L) - sw / 2;
    float post_top = r->y - post_h, board_top = post_top - sh;
    fill(x, board_top, sw, sh, rec[6]);
    fill(x + sw - pw, post_top, pw, post_h, rec[8]);
    fill(x, post_top, pw, post_h, rec[8]);

    float save_w = line_w;
    u16 mx = rec[10], my = rec[11], x0 = mx;
    u8 text[2 * (0x2D - 13)];
    for (int n = 0; n < 0x2D - 13; n++) {
        text[2 * n] = (u8)rec[13 + n];
        text[2 * n + 1] = (u8)(rec[13 + n] >> 8);
    }
    for (size_t p = 0; p < sizeof text; p++) {
        s8 c = (s8)text[p];
        if (c < 10) break;
        if (c == 10) {
            mx = x0;
            my = (u16)(my + rec[3]);
            continue;
        }
        u16 g = rd16(fnt, (u16)(((u16)(u8)c - 0x20) << 1));
        if (g != 0) {
            for (int guard = 0; guard < 64; guard++) {
                if (rd8(fnt, g) == 0xFF) break;
                double ax = ((u16)(rd8(fnt, g) + mx)) * W / 512.0 + x;
                double ay = ((u16)(rd8(fnt, (u16)(g + 1)) + my)) * W / 512.0 + board_top;
                double bx = ((u16)(rd8(fnt, (u16)(g + 2)) + mx)) * W / 512.0 + x;
                double by = ((u16)(rd8(fnt, (u16)(g + 3)) + my)) * W / 512.0 + board_top;
                g = (u16)(g + 4);
                line((float)ax, (float)ay, (float)bx, (float)by, rec[4]);
            }
        }
        mx = (u16)(mx + rec[2]);
    }
    line_w = save_w;
}

#define SCENERY_ROWS 44                  /* the original draws scenery on rows 0..43 */

static void scenery(int j, double zlim_near)                               /* §4.10 */
{
    const EnhRow *r = &S->rows[j];
    u16 k = (u16)(r->phase & 0x7F);
    s8 t = DSC((u16)(DS_dat_scenery_type + k));
    if (t < 0) return;
    if ((u8)t < 0x50) {
        u16 base = (u16)(DS_scenery_handles + ((u16)(u8)t << 2));
        u16 di = (u16)(base + 4 * s5_cur);
        if (DSW((u16)(di + 2)) != 0) {
            s16 off = DSC((u16)(DS_dat_scenery_offset + k));
            off = (s16)(off >= 0 ? off + 2 : off - 2);
            float x = off * r->W / 8;
            x += x > 0 ? r->R : r->L;
            if (group_cut_off(base, &fam5)) {
                /* opaque where the original draws it, fading in over the 5 units beyond */
                double a = (zlim_near + 5 - r->z) / 5;
                if (a <= 0) return;
                if (a < cur_alpha) cur_alpha = (float)a;
            }
            float ks = SCALE5(base, r->W);
            and_h(di, x, r->y, ks);
            or_h((u16)(di + 0x140), x, r->y, ks);
            return;
        }
    }
    if ((u8)t < 0x1E) return;
    text_sign(j, k);
}

static void poles(int j)
{
    const EnhRow *r = &S->rows[j];
    if (r->phase & 0x0F) return;
    if (r->state & 0x80) {
        rib(j);
        return;
    }
    float cy = r->y - os_cur;
    u16 base = ROAD_H(ROAD_pal0);
    float k = SCALE4(base, r->W);
    u16 bx = (u16)(base + 4 * s4_cur);
    float q = r->W / 4;
    float bp = r->L - q, dx = r->R - 1 + q;
    if (dx >= 0 && dx < VIEW_W) { and_h(bx, dx, cy, k); or_h((u16)(bx + 0x10), dx, cy, k); }
    if (bp >= 0 && bp < VIEW_W) { and_h(bx, bp, cy, k); or_h((u16)(bx + 0x10), bp, cy, k); }
}

/* ------------------------------------------------------------------------------------------------ */
/* cars                                                                                             */

static const EnhCar *car_list;
static int car_n, car_next;

static int carscale_at(double z)
{
    int i = (int)floor(z) - 4;
    if (i < 0) i = 0;
    if (i > 59) return 0;
    return DSB((u16)(DS_carscale_front + 2 * i));
}

/* road values at fractional row jf (row j = unit car_unit + j) */
static bool road_at(double jf, double *z, double *X, double *H, double *Rw)
{
    if (jf < 0 || jf >= nrows) return false;
    int j = (int)jf;
    double t = jf - j;
    const EnhRow *a = &S->rows[j], *b = &S->rows[j + 1];
    *z = a->z + t;
    *X = a->X + (b->X - a->X) * t;
    *H = a->H + (b->H - a->H) * t;
    /* the right edge including widening, interpolated in world units */
    double ra = (a->R - 125.0) * a->z, rb = (b->R - 125.0) * b->z;
    *Rw = ra + (rb - ra) * t;
    return true;
}

double enh_scene_screen_x(double s_unit, double lat)
{
    double z, X, H, Rw;
    if (!road_at(s_unit - car_unit, &z, &X, &H, &Rw)) return NAN;
    return 125.0 + (X + lat) * KX / z;
}

static void draw_car(const EnhCar *c, double zlim)
{
    double jf = c->s - car_unit;                           /* fractional row */
    double z, X, H, Rw;
    if (!road_at(jf, &z, &X, &H, &Rw)) return;
    float save = cur_cy1;
    int jn = (int)ceil(jf) - 1;                            /* rows nearer than the car: 0..jn */
    if (jn < 0) jn = 0;
    float clip = S->rows[jn].clip;
    float yroad = (float)(51.0 + H * KY / z);
    if (yroad < clip) clip = yroad;
    cur_cy1 = clip;
    cur_alpha = fade(z, zlim);
    float W = (float)(KW / z);
    int s = carscale_at(z);
    float y = yroad - 1;
    float x = (float)(125.0 + (X + c->lat) * KX / z);
    bool front = true;
    switch (c->kind) {
    case ENH_CAR_TRAFFIC: {
        u16 bx = (u16)(c->type - 1);
        u16 e = (u16)((((bx & 3) << 4) + ((bx & 4) << 1)) << 1);
        u16 base = (u16)(DS_traffic1_handles + (e << 2));
        float k = group_scale(base, 4, s, &famcar, W);
        and_h((u16)(base + 4 * s), x, y, k);
        or_h((u16)(base + 0x20 + 4 * s), x, y, k);
        break;
    }
    case ENH_CAR_OPP: {
        u16 base = (u16)(DS_opp_road_handles + (DSB(DS_opp_crash_timer) != 0 ? 0x40 : 0));
        float k = group_scale((u16)(base + 0x20), 4, s, &famcar, W);
        and_h((u16)(base + 0x20 + 4 * s), x, y, k);
        or_h((u16)(base + 4 * s), x, y, k);
        if (front && DSB(DS_opp_braking) != 0) xor_h((u16)(DS_opp_road_handles + 0x80 + 4 * s), x, y, k);
        break;
    }
    case ENH_CAR_COP: {
        float k = group_scale((u16)(DS_cop_car_handles + 0x40), 4, s, &famcar, W);
        and_h((u16)(DS_cop_car_handles + 0x40 + 4 * s), x, y, k);
        or_h((u16)(DS_cop_car_handles + 0x60 + 4 * s), x, y, k);
        if (DSB(DS_cop_braking) != 0) xor_h((u16)(DS_cop_extra_handles + 0x20 + 4 * s), x, y, k);
        if (DSW(DS_stage_time) & 1) xor_h((u16)(DS_cop_extra_handles + 4 * s), x, y, k);
        break;
    }
    default: {                                             /* parked police car at the right edge */
        u16 fr = (u16)(DSW(DS_sim_tick10) & 0x0C);
        u16 base = (u16)(DS_cop_extra_handles + 0x40 + fr);
        float k = group_scale((u16)(base + 0x80), 16, s, &famcar, W);
        float xr = (float)(125.0 + Rw / z);
        and_h((u16)(base + 0x80 + 16 * s), xr, y, k);
        or_h((u16)(base + 16 * s), xr, y, k);
        break;
    }
    }
    cur_cy1 = save;
    cur_alpha = 1;
}

/* draw_tunnel_walls between the far end and the entrance; ends hidden behind a crest are taken at the crest
 * (the same values as the original's when they are visible) */
static void walls(float in_l, float in_r, float out_sy, int out_row, float in_sy, int in_row)
{
    if (S->rows[out_row].clip < out_sy) out_sy = S->rows[out_row].clip;
    if (S->rows[in_row].clip < in_sy) in_sy = S->rows[in_row].clip;
    EnhCmd *c = cmd(CMD_WALLS);
    if (!c) return;
    c->x0 = in_l;
    c->x1 = in_r;
    c->y0 = out_sy;
    c->y1 = in_sy;
}

static void cars_between(int j, double zlim)
{
    /* cars farther than row j - 1 and not farther than row j */
    while (car_next < car_n) {
        const EnhCar *c = &car_list[car_next];
        double jf = c->s - car_unit;
        if (jf <= j - 1) break;
        car_next++;
        if (jf <= j) draw_car(c, zlim);
    }
}

static int car_cmp(const void *pa, const void *pb)
{
    const EnhCar *a = pa, *b = pb;
    if (a->s != b->s) return a->s > b->s ? -1 : 1;         /* far first */
    return a->order - b->order;
}

/* ------------------------------------------------------------------------------------------------ */

static void objects(double zlim_obj, double zlim_scn)
{
    for (int j = nrows; j >= 0; j--) {
        const EnhRow *r = &S->rows[j];
        j_cur = j;
        st_cur = r->state;
        set_ceiling_clip(j);
        float W = r->W;
        W_cur = W;
        int os = (int)W >> 3;
        if (os >= 0x1F) os = 0x1F;
        os_cur = W / 8;
        s5_cur = (os >= 16 ? 16 : os) / 4;
        s4_cur = (os >> 1) / 4;
        cur_cy1 = r->clip;
        cur_alpha = 1;
        line_w = 1;
        if (!S->style && (S->r0_any & 0x80) && S->tunnel_out_found && j > S->tunnel_out_row) {
            cur_cx0 = S->tunnel_out_l;
            cur_cx1 = S->tunnel_out_r;
        } else {
            cur_cx0 = 0;
            cur_cx1 = VIEW_W;
        }

        /* 1. road markings of the scanlines between this row and the nearer one */
        if (j >= 1) {
            EnhCmd *c = cmd(CMD_MARK);
            if (c) c->a = j;
        }

        /* 2. left cliff, 3. right cliff */
        if (j > CUT_ROWS && !(st_cur & 0x80)) {
            if (st_cur & 0x40) far_cliff(j, true);
            if (st_cur & 0x08) far_cliff(j, false);
        }
        if (st_cur & 0x40) {
            if (j == S->left_cut_row && !(st_cur & 0x80)) cliff_wall(j, true);
            if (!(st_cur & 0x80) && j <= 23 && j < S->left_cut_row) cliff_deco(j, true);
        }
        if (st_cur & 0x08) {
            if (j == S->right_cut_row && !(st_cur & 0x80)) cliff_wall(j, false);
            if (!(st_cur & 0x80) && j <= 23 && j < S->right_cut_row) cliff_deco(j, false);
        }
        cur_cy0 = 0;

        /* 4. tunnels */
        if (S->r0_any & 0x80) {
            EnhTunnel near_t = { S->tunnel_in_row, S->tunnel_out_row, S->tunnel_in_sy, S->tunnel_in_top,
                                 S->tunnel_out_sy, S->tunnel_out_top, S->tunnel_in_l, S->tunnel_in_r,
                                 S->tunnel_out_l, S->tunnel_out_r, true };
            if (S->tunnel_nearest) tunnel_mouths(j, &near_t, true);
            if (!S->style) cur_cy0 = S->tunnel_ceiling;
            for (int e = 0; e < S->nextra; e++) tunnel_mouths(j, &S->extra[e], false);
            cur_cy0 = 0;
        }
        set_ceiling_clip(j);

        double z = r->z;
        cur_alpha = fade(z, zlim_obj);
        line_w = W >= 19 ? 1.0f : W / 19.0f;

        /* 5. tunnel lights every 16 units */
        if (!S->style && ((u8)(r->phase + 8) & 0x0F) == 0 && (st_cur & 0x80)) {
            float cy = r->y - W / 2;
            if (cy >= 0) {
                u16 base = SCN_H(80);
                float k = SCALE5(base, W);
                or_h((u16)(base + 4 * s5_cur), r->cx, cy, k);
            }
        }

        /* 6. road object */
        if (r->obj != 0 && r->obj < 0x15) road_object(j, r->obj);

        /* 7. scenery (the ring holds ENH_SCENERY_AHEAD units ahead of the last simulation step) */
        int ahead = r->unit - step_unit - 1;
        if (ahead <= ENH_SCENERY_AHEAD && z < zlim_scn) {
            cur_alpha = fade(z, zlim_scn);
            scenery(j, SCENERY_ROWS + 4);
            cur_alpha = fade(z, zlim_obj);
        }

        /* 8. poles */
        poles(j);

        /* 9-12. cars between this row and the nearer one */
        cur_alpha = 1;
        line_w = 1;
        cars_between(j, zlim_obj);
    }
}

void enh_scene_build(const EnhView *v, const EnhCar *cars, int ncars)
{
    if (!fam4.n) {
        family_init(&fam4, 4, variant4);
        family_init(&fam5, 5, variant5);
    }
    family_init(&famcar, 8, variantcar);
    S->ncmds = 0;
    S->col_left = (u8)(DSW(DS_scene_words) & 15);
    S->col_right = (u8)(DSW(DS_col_right) & 15);
    S->col_shoulder = (u8)(DSW(DS_col_shoulder) & 15);
    S->col_sky = (u8)(DSW(DS_col_sky) & 15);
    S->col_far = (u8)(DSW(DS_col_far) & 15);
    project(v);
    cut_lines();
    ground_pairs();

    cur_cx0 = 0;
    cur_cx1 = VIEW_W;
    sky(v);
    cur_cy0 = 0;
    cur_cy1 = VIEW_H;
    cur_alpha = 1;
    cmd(CMD_GROUND);
    if (S->r0_any & 0x80) {
        if (S->tunnel_nearest)
            walls(S->tunnel_in_l, S->tunnel_in_r, S->tunnel_out_sy, S->tunnel_out_row, S->tunnel_in_sy, S->tunnel_in_row);
        /* tunnels beyond the nearest one are seen through its far end */
        if (!S->style) cur_cy0 = S->tunnel_ceiling;
        for (int e = 0; e < S->nextra; e++)
            walls(S->extra[e].in_l, S->extra[e].in_r, S->extra[e].out_sy, S->extra[e].out_row, S->extra[e].in_sy,
                  S->extra[e].in_row);
        cur_cy0 = 0;
    }

    static EnhCar sorted[ENH_MAX_CARS];
    car_n = ncars < ENH_MAX_CARS ? ncars : ENH_MAX_CARS;
    memcpy(sorted, cars, (size_t)car_n * sizeof *sorted);
    qsort(sorted, (size_t)car_n, sizeof *sorted, car_cmp);
    car_list = sorted;
    car_next = 0;

    double zlim_obj = nrows + 2;
    double zlim_scn = ENH_SCENERY_AHEAD < nrows + 2 ? ENH_SCENERY_AHEAD : nrows + 2;
    objects(zlim_obj, zlim_scn);
}
