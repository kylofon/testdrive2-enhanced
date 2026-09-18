/* Enhanced renderer: the front view and the rear-view mirror in continuous depth (ENHANCED.md "Smooth
 * motion", "Projection", "Draw distance", "Mirror").
 *
 * This is the faithful view code (game/scene_project.c, scene_draw.c, scene_objects.c) evaluated in
 * floating point for rows at continuous depths, with more rows, producing a display list in original
 * buffer coordinates instead of drawing. The structure, the variables and the drawing order follow the
 * original so that every feature (drop-offs, cliffs, tunnels, bands, objects, cars) behaves the same.
 * Like the original, one implementation serves both views (the scene S holds the view's parameters); the
 * mirror's differences are marked `front` / `!front`. */
#include "enh_internal.h"
#include "../game/scene.h"
#include "../platform/res.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

EnhScene enh_sc, enh_mc;

#define ROAD0    0x3B51                  /* first road byte */
#define DAT_END  0x52C8                  /* end of the stage image */
#define FADE     0.1                     /* objects fade in over the last 10 % of their distance */

static EnhScene *S = &enh_sc;
#define V_W ((float)S->vw)
#define V_H ((float)S->vh)
#define KX  (S->kx)
#define KY  (S->ky)
#define KW  (S->kw)
#define CUT_ROWS (S->orig_rows)                  /* rows that place the cliff and drop-off cut lines (the original's) */

/* view parameters (scene_project.c scene_front_view / scene_mirror_view; tables xs = kx * 65536 / depth,
 * ys = ky * 65536 / depth, w = kw / depth, depth = row + depth0) */
static void view_setup(EnhScene *sc, bool front)
{
    sc->front = front;
    if (front) {
        sc->vw = VIEW_W; sc->vh = VIEW_H; sc->horizon = 51; sc->centre = 125;
        sc->kx = 195256.0 / 65536.0; sc->ky = 140672.0 / 65536.0; sc->kw = 1200.0; sc->lat_k = 1;
        sc->orig_rows = 60; sc->depth0 = 4; sc->cut_offset = 22; sc->sky_cut = 15; sc->portal_y = 0x5B;
        sc->sky_handles = DS_scenery_sky_handles; sc->carscale = DS_carscale_front; sc->band_dx = 31;
        sc->scenery_rows = 44;
    } else {
        sc->vw = MIRROR_W; sc->vh = MIRROR_H; sc->horizon = 8; sc->centre = 40;
        sc->kx = 106416.0 / 65536.0; sc->ky = 57324.0 / 65536.0; sc->kw = 360.0; sc->lat_k = 0.5;
        sc->orig_rows = 25; sc->depth0 = 6; sc->cut_offset = 6; sc->sky_cut = 3; sc->portal_y = 0x10;
        sc->sky_handles = DS_mirror_scenery_handles; sc->carscale = DS_carscale_mirror; sc->band_dx = 29;
        sc->scenery_rows = 25;
    }
}

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

/* The original's integrators (project_rows) for a front view whose car is at unit `org` and whose heading
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

/* The mirror's (project_mirror_rows): walking backwards from the car's unit org with the heading -yaw0.
 * Index 0 is unit org + 1 (one row nearer than the original's row 0, level and straight); index k + 1 is
 * the original's row k, unit org - k, whose record is applied before its row is placed. */
static void integrate_mirror(int org, double yaw0, int n, double *H, double *X)
{
    double pacc = 0, hacc = -yaw0, h = 80, x = 0;         /* DS:2A1E start height */
    H[0] = 80;
    X[0] = 0;
    for (int k = 1; k < n; k++) {
        const u8 *r = road_rec(org - (k - 1));
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
    u16 dx = (u16)(u + S->band_dx);                      /* walk_ptr - 0x3B33 */
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

static float cur_cy0, cur_cy1, cur_cx0, cur_cx1 = 320, cur_alpha = 1;

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

static EnhCmd *fill_cmd(float x, float y, float w, float h)
{
    if (!(w > 0) || !(h > 0) || cur_alpha <= 0) return NULL;
    EnhCmd *c = cmd(CMD_FILL);
    if (!c) return NULL;
    c->x0 = x; c->y0 = y; c->x1 = x + w; c->y1 = y + h;
    return c;
}

static void fill(float x, float y, float w, float h, u16 colour)          /* 06c9:89a2 */
{
    EnhCmd *c = fill_cmd(x, y, w, h);
    if (c) c->colour = (u8)(colour & 15);
}

/* a fill in an extended colour */
static void fill_ext(float x, float y, float w, float h, u8 colour)
{
    EnhCmd *c = fill_cmd(x, y, w, h);
    if (c) c->colour = colour;
}

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

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
typedef struct { Family f4, f5, car; bool init; } Families;
static Families fams[2];                  /* front, mirror */
#define fam4   (fams[S->front ? 0 : 1].f4)
#define fam5   (fams[S->front ? 0 : 1].f5)
#define famcar (fams[S->front ? 0 : 1].car)

static int orig_w(int i) { return (int)(S->kw / (i + S->depth0)); }
static int variant4(int i) { int os = orig_w(i) >> 3; if (os > 31) os = 31; return (os >> 1) / 4; }
static int variant5(int i) { int os = orig_w(i) >> 3; if (os > 31) os = 31; return (os >= 16 ? 16 : os) / 4; }
static int variantcar(int i) { return DSB((u16)(S->carscale + 2 * i)) & 7; }

/* variant k's nominal half-width: the width at the centre of the original rows that select it */
static void family_init(Family *f, int n, int (*variant)(int))
{
    f->n = n;
    for (int k = 0; k < n; k++) {
        int lo = 99, hi = -1;
        for (int i = 0; i < S->orig_rows; i++)
            if (variant(i) == k) { if (i < lo) lo = i; if (i > hi) hi = i; }
        f->ok[k] = hi >= 0;
        f->wnom[k] = hi >= 0 ? KW / sqrt((lo + (double)S->depth0) * (hi + S->depth0 + 1.0)) : 0;
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
    bool front = S->front;
    nrows = front ? enh_rows_setting : ENH_MIRROR_ROWS(enh_rows_setting);
    S->nrows = nrows;
    car_unit = (int)floor(v->s);
    frac = v->s - car_unit;
    step_unit = (int)v->pos - ROAD0;

    /* views at the car's unit (a) and the next one (b), each with its view_yaw and lateral, blended by
     * the sub-unit fraction. Front row j: unit car_unit + j, the original's row j - 1 of view a and row
     * j - 2 of view b. Mirror row j: unit car_unit + 1 - j, the original's row j - 1 of view a and row j
     * of view b. */
    int n = nrows + 3;
    if (front) {
        integrate(car_unit, v->yaw_a, n, H_a, X_aa);
        integrate(car_unit + 1, v->yaw_b, n, H_b, X_ba);
    } else {
        integrate_mirror(car_unit, v->yaw_a, n, H_a, X_aa);
        integrate_mirror(car_unit + 1, v->yaw_b, n, H_b, X_ba);
    }

    /* region state at the car's unit (the simulation's DS:5491 moved on to it); the mirror's rows toggle
     * it from the car's unit backwards, so its row j has the state before its unit */
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
        double f = frac;
        int u;
        if (front) {
            u = car_unit + j;
            r->z = j + 3 - f;
            r->H = (1 - f) * H_a[j + 1] + f * H_b[j];
            r->X = (1 - f) * (X_aa[j + 1] - v->lat_a) + f * (X_ba[j] - v->lat_b);
        } else {
            u = car_unit + 1 - j;
            r->z = j + 5 + f;
            r->H = (1 - f) * H_a[j] + f * H_b[j + 1];
            r->X = (1 - f) * (X_aa[j] - v->lat_a * S->lat_k) + f * (X_ba[j + 1] - v->lat_b * S->lat_k);
        }
        r->unit = u;
        double z = r->z;
        r->cx = (float)(S->centre + r->X * KX / z);
        r->y = (float)(S->horizon + r->H * KY / z);
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
    S->top_sy = S->top_sy_near = V_H;
    S->top_row = 1;
    S->left_cut_x = 0;
    S->right_cut_x = V_W;
    S->left_sky_x = V_W;
    S->right_sky_x = 0;
    S->left_sky_y = S->right_sky_y = 0;
    S->left_cut_row = S->right_cut_row = S->left_sky_row = S->right_sky_row = 1;
    S->tunnel_in_row = S->tunnel_out_row = S->tunnel_ceiling_row = 1;
    S->tunnel_in_sy = V_H;
    S->tunnel_in_top = S->tunnel_out_sy = S->tunnel_out_top = S->tunnel_ceiling = 0;
    S->tunnel_in_l = 0;
    S->tunnel_in_r = V_W;
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
    S->rows[0].clip = V_H;
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
        if (near && r->y < S->top_sy_near) S->top_sy_near = r->y;
        r->clip = S->top_sy;

        float ys = r->y < V_H ? r->y : V_H;
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
            } else if ((tstage == 2 || tstage == 0) && S->nextra < ENH_MAX_TUNNELS) {
                /* a tunnel beyond the original's rows, or beyond the nearest one: drawn with its own
                 * entrance until the original's portal takes over (tunnel_mouths) */
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
    S->rows[1].clip = V_H;
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
        S->left_sky_x = S->left_sky_x < 0 ? 0 : S->left_sky_x > V_W ? V_W : S->left_sky_x;
    }
    if (bl & 0x04) {
        S->right_sky_y = S->rows[S->right_sky_row].y;
        S->right_sky_x = S->right_sky_x < 0 ? 0 : S->right_sky_x > V_W ? V_W : S->right_sky_x;
    }
    float d = S->left_cut_x;
    if ((bl & 0x40) && !(S->left_cut_state & 0x80)) d -= S->cut_offset;
    S->left_cut_x = d <= 0 ? 0 : d < V_W ? d : V_W;
    d = S->right_cut_x;
    if ((bl & 0x08) && !(S->right_cut_state & 0x80)) d += S->cut_offset;
    S->right_cut_x = d <= 0 ? 0 : d < V_W ? d : V_W;
    if (!(bl & 0x80)) return;
    if (!out_found && S->tunnel_nearest) {
        S->tunnel_out_sy = S->tunnel_out_top = S->top_sy;
        S->tunnel_out_row = nrows;
    }
    if (S->tunnel_ceiling_row < S->top_row && S->tunnel_ceiling > S->top_sy) S->top_sy = S->tunnel_ceiling;
    if (S->top_sy_near < S->top_sy) S->top_sy_near = S->top_sy;
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
        else if (x > V_W) x = V_W;
    }
    if (f & 0xC0) {
        if (x > V_W) return V_W;
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
    float clip = V_H + 64;                              /* below the window: the nearest pair */
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

/* y of the road at depth z (a fixed row of the original) */
static float row_y_at_depth(double z)
{
    double jf = S->front ? z - 3 + frac : z - 5 - frac;
    if (jf < 0) jf = 0;
    if (jf > nrows) jf = nrows;
    int j0 = (int)jf;
    if (j0 >= nrows) return S->rows[nrows].y;
    return S->rows[j0].y + (S->rows[j0 + 1].y - S->rows[j0].y) * (float)(jf - j0);
}

static void sky(const EnhView *v)
{
    float top = S->top_sy;
    u16 skyc = S->col_sky;
    cur_cy0 = 0;
    cur_cy1 = V_H;
    cur_alpha = 1;
    if (!S->style) {
        u8 bl = S->r0_any;
        if ((bl & 0x80) && S->tunnel_nearest) {
            float di = S->tunnel_out_l, bp = S->tunnel_out_r;
            if (S->sky_state & 0x80) {
                fill(di, S->tunnel_in_top, bp - di, S->tunnel_out_top - S->tunnel_in_top, 0);
                return;
            }
            /* the original leaves the cliff's side of the cut lines to its cliff fill; the rock faces
             * are drawn over the sky here */
            float tt = S->tunnel_in_top, ceil = S->tunnel_ceiling;
            fill(di, ceil, bp - di, top - ceil, skyc);
            fill(di, tt, bp - di, ceil - tt, 0);
            return;
        }
        if (bl & 0x48) {                                   /* no mountains beside a cliff */
            fill(0, 0, V_W, top, skyc);
            return;
        }
    }
    fill(0, 0, V_W, top, skyc);
    if (S->backdrop_off) return;
    /* The mountains stand on the horizon of the original's rows, not on the highest point of all of them:
     * with the longer draw distance a climb 60 to 180 units ahead would otherwise lift them into the sky.
     * The ground of those far rows is drawn after this and covers them, as a hill in front of them would. */
    top = S->top_sy_near;
    u16 hb = S->sky_handles;
    if (DSW((u16)(hb + 8 * 4 + 2)) == 0) return;          /* mtn0 / rmt0 */
    double hy = v->yaw * 8.0 / 256.0;                      /* (s8)((view_yaw << 3) >> 8) */
    if (!S->front) {
        /* rmt0-2 at half the scroll, on the original's farthest row (depth 30); no clouds */
        double dm = fmod(v->heading - hy, 1024.0);
        if (dm < 0) dm += 1024.0;
        dm /= 2;
        float my = row_y_at_depth(30);
        copy_h((u16)(hb + 8 * 4), (float)(dm - 512), my, 1);
        copy_h((u16)(hb + 10 * 4), (float)(dm - 250), my, 1);
        copy_h((u16)(hb + 9 * 4), (float)(dm - 150), my, 1);
        copy_h((u16)(hb + 8 * 4), (float)dm, my, 1);
        return;
    }
    double di = fmod(-(v->heading - hy), 1024.0);
    if (di < 0) di += 1024.0;
    copy_h((u16)(hb + 8 * 4), (float)(di - 1024), top, 1);
    copy_h((u16)(hb + 10 * 4), (float)(di - 500), top, 1);
    copy_h((u16)(hb + 9 * 4), (float)(di - 300), top, 1);
    copy_h((u16)(hb + 8 * 4), (float)di, top, 1);
    if (DSW((u16)(hb + 11 * 4 + 2)) == 0) return;         /* clo1 */
    di = fmod(-(v->cloud - hy), 1024.0);
    if (di < 0) di += 1024.0;
    float cy = row_y_at_depth(63) - 30;                    /* the original's farthest row */
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

/* Rock faces (enh_raster.c do_face, after Test Drive Enhanced). The original draws the near cliff as one
 * fill from its cut line to the edge of the view and everything above it, with its cliff-edge sprite at the
 * cut row. Here every pair of cliff rows gets a face along the outer road edge, leaning outwards like that
 * sprite, so the face follows the road in bends and over hills. The rock is an object of the world: each
 * cliff unit has a height above its road edge (cliff_rise) that does not depend on the view, projected like
 * everything else, so a piece of rock only grows by perspective as the car approaches. It is at least
 * RIDGE_MIN, which reaches the top of the view at the original's distance on level ground (where the
 * original's fill covers everything above), and varies slowly along the road as a ridge with a skyline. Far
 * away the ridge hides the mountains behind it; it fades out (dithered) over the last part of the view. */
#define RIDGE_MIN 1650.0                  /* lowest rock above the road edge (height units; the eye is 80) */
#define RIDGE_VAR 1000.0                  /* plus up to this much, varying along the road */

static double zlim_rock;                  /* the objects' distance: far rock fades out before it */

static double ridge_h01(u32 x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (x & 0xFFFFFF) / 16777216.0;
}

/* smooth value noise of the road position in [0, 1) */
static double unit_noise(double u, u32 seed)
{
    double fl = floor(u), t = u - fl;
    u32 a = (u32)(s32)fl;
    double va = ridge_h01(a * 0x9E3779B1u + seed), vb = ridge_h01((a + 1) * 0x9E3779B1u + seed);
    t = t * t * (3 - 2 * t);
    return va + (vb - va) * t;
}

/* height of the rock above the road edge of unit u (height units) */
static double cliff_rise(int u)
{
    return RIDGE_MIN + RIDGE_VAR * (0.65 * unit_noise(u / 37.0, 0x51D6E) + 0.35 * unit_noise(u / 11.0, 0x2A7F3));
}

/* the face height of a row on the screen (px) */
static float cliff_height(const EnhRow *r)
{
    return (float)(cliff_rise(r->unit) * KY / r->z);
}

/* the rock face along the outer edge between cliff row j and the nearer one; the nearest face of a side,
 * if it is within the original's rows, also covers everything outwards of it (as the original's fill) */
static void cliff_face(int j, bool left, bool nearest)
{
    float hf = cliff_height(&S->rows[j]), hn = cliff_height(&S->rows[j - 1]);
    if (!(hf > 0) && !(hn > 0)) return;
    EnhCmd *c = cmd(CMD_FACE);
    if (!c) return;
    c->a = j;
    c->op = left;
    c->x0 = hf;
    c->x1 = hn;
    c->w = nearest ? 1.0f : 0.0f;
}

/* rock colour at depth z (tunnel portals and hills, drawn as fills) */
static u8 rock_at(double z)
{
    return (u8)(EXT_ROCK + (int)(enh_rock_haze(z) * (ENH_HAZE - 1) + 0.5));
}

static void cliff_deco(int j, bool left)
{
    const EnhRow *r = &S->rows[j];
    u16 w = DSW((u16)(DS_cliff_deco_pattern + (((u8)(r->phase << 1)) & 0x1E)));
    if ((u8)w == 0) return;
    float x = left ? r->ol : r->or_;
    if (!(x >= 0 && x < V_W)) return;
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
    float wd = V_W;
    u16 hb = S->sky_handles;
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
    if (!nearest && !style) {
        /* A tunnel beyond the original's rows: the hill it goes into, drawn like a rock face - as high
         * above the road as the rock of that unit (cliff_rise, fixed in the world) and sloping down to both
         * sides - with the mouth cut out of it. By the original's distance it reaches the top of the view,
         * where the original's portal (which fills everything above and beside the mouth) takes over. */
        float h = cliff_height(r);
        if (!(h > 0)) return;
        float top = r->y - h;
        if (top >= clip) return;
        float wide = (in_r - in_l) / 2 + h * (float)CLIFF_LEAN;
        float save_alpha = cur_alpha;
        cur_alpha = fade(r->z, zlim_rock);
        u8 rock = rock_at(r->z);
        int ns = (int)((clip - top) * 2);             /* slices, narrowing towards the top: a smooth slope */
        ns = ns < 4 ? 4 : ns > 48 ? 48 : ns;
        for (int k = 0; k < ns; k++) {
            float y1 = clip - (clip - top) * k / ns, y0 = clip - (clip - top) * (k + 1) / ns;
            float w = wide * (ns - k - 0.5f) / ns;
            float x0 = in_l - w, x1 = in_r + w;
            float mouth_top = in_top;
            if (y0 >= mouth_top) {                     /* above the mouth: one slice */
                fill_ext(x0, y0, x1 - x0, y1 - y0, rock);
            } else {
                if (y1 > mouth_top) {
                    fill_ext(x0, mouth_top, in_l - x0, y1 - mouth_top, rock);
                    fill_ext(in_r, mouth_top, x1 - in_r, y1 - mouth_top, rock);
                    y1 = mouth_top;
                }
                if (y1 > y0) fill_ext(x0, y0, x1 - x0, y1 - y0, rock);
            }
        }
        cur_alpha = save_alpha;
        return;
    }
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
    u8 rock = rock_at(r->z);                               /* the original's colour 6, hazed */
    if (!(r->state & 0x40)) {                              /* portal A */
        float bp = in_l, di = S->left_sky_x, px;
        bool first;
        if (j > S->left_sky_row) first = true;
        else if (di == 0) first = false;
        else { di += S->sky_cut; first = bp <= di; }
        if (first) {
            fill(0, 0, bp, top_sy, skyc);
            px = r->L;
        } else {
            fill_ext(di, 0, bp - di, clip, rock);
            bp = di;
            if (bp == 0) goto walls_a;
            fill(0, 0, bp, top_sy, skyc);
            px = bp;
        }
        and_h((u16)(hb + 2 * 4), px, S->portal_y, 1);      /* rcfB / rcfD */
        or_h((u16)(hb + 3 * 4), px, S->portal_y, 1);       /* rcfb / rcfd */
    walls_a:
        fill_ext(bp, 0, wd - bp, in_top, rock);
        fill_ext(in_r, in_top, wd - in_r, clip - in_top, rock);
    } else {                                               /* portal B */
        float bp = in_r, di = S->right_sky_x, px;
        bool first;
        if (j > S->right_sky_row) first = true;
        else if (di == wd) first = false;
        else { di -= S->sky_cut; first = bp >= di; }
        if (first) {
            fill(bp, 0, wd - bp, top_sy, skyc);
            px = r->R;
        } else {
            fill_ext(in_r, 0, di - in_r, clip, rock);
            bp = di;
            fill(bp, 0, wd - bp, top_sy, skyc);
            px = bp;
        }
        and_h((u16)(hb + 6 * 4), px, S->portal_y, 1);      /* lcfB / lcfD */
        or_h((u16)(hb + 7 * 4), px, S->portal_y, 1);       /* lcfb / lcfd */
        fill_ext(0, 0, bp, in_top, rock);
        fill_ext(0, in_top, in_l, clip - in_top, rock);
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
        if (S->front) {                                    /* the mirror never draws the sign image */
            or_h((u16)(di + 0x90), R, dy, k);
            or_h((u16)(di + 0x90), L, dy, k);
        }
    } else if (o == 10 || o == 12) {                       /* white band across the road */
        float save = cur_cy1;
        cur_cy1 = V_H;
        float y0, y1;
        if (S->front) {                                    /* from this row to three rows nearer */
            y1 = j - 3 >= 1 ? S->rows[j - 3].y : V_H;
            y0 = y < V_H ? y : V_H;
        } else {                                           /* from three rows farther to this row */
            y0 = j + 3 <= nrows ? S->rows[j + 3].y : S->top_sy;
            y1 = y < V_H ? y : V_H;
        }
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
            if (!S->front) return;                         /* the mirror draws no letters */
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

static void text_sign(int j, s8 t, s8 soff)                                /* §4.10 SGN signs */
{
    const EnhRow *r = &S->rows[j];
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
    float x = (float)(soff * W / 8.0);
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

/* Wider scenery: the view is wider than the original's and its sides were empty, so a tree or shrub the
 * original places gets one or two more of its kind (or of a neighbouring unit's) further out on the same
 * side, EXTRA_OUT eighths of the road's half-width and more beyond it and a little farther away (up to
 * EXTRA_DEPTH units). Everything is derived from the road unit, so they stay put, look the same in the
 * mirror and change nothing in the simulation. Only trees and shrubs (mostly green sprites: no houses, rocks
 * or signs), none beside cliffs, drop-offs and tunnels, none beyond the far-right band (water), and no
 * extra redwoods (the cut-off trunks). */
#define EXTRA_OUT   6                   /* first extra: this many eighths of W further out */
#define EXTRA_STEP  5                   /* the next one */
#define EXTRA_DEPTH 0.45                /* depth offset: up to this many units farther */

static u32 unit_hash(int u)
{
    u32 x = (u32)u * 0x9E3779B1u + 0x7F4A7C15u;
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* a tree or shrub: mostly green (colours 2 and 10) in the image of its largest variant (not a house, a
 * rock or a sign); remembered in the decoded sprite */
static bool greenery(u16 base)
{
    for (int k = 4; k >= 0; k--) {
        EnhSprite *s = (EnhSprite *)enh_sprite(hnd_at((u16)(base + 4 * k + 0x140)));
        if (!s) continue;
        if (s->green == 0) {
            int n = 0, g = 0;
            for (int i = 0; i < s->w * s->h; i++) {
                u8 v = s->bits[i];
                if (!(s->touch[EOP_OR] & (1 << v))) continue;
                u8 c = s->lut[EOP_OR][v << 4];
                n++;
                if (c == 2 || c == 10) g++;
            }
            s->green = n > 0 && g * 10 >= n * 3 ? 2 : 1;
        }
        return s->green == 2;
    }
    return false;
}

/* a tree or shrub of scenery type t (not a text sign, not a redwood trunk): its handle group, or 0 */
static u16 plain_sprite(s8 t)
{
    if (t < 0 || (u8)t >= 0x50) return 0;
    u16 base = (u16)(DS_scenery_handles + ((u16)(u8)t << 2));
    if (DSW((u16)(base + 4 * s5_cur + 2)) == 0 || group_cut_off(base, &fam5) || !greenery(base)) return 0;
    return base;
}

static void scenery_extras(int j, s8 t, s16 off)
{
    const EnhRow *r = &S->rows[j], *f = &S->rows[j + 1 <= nrows ? j + 1 : j];
    bool right = off > 0;
    u8 blocked = right ? 0x8C : 0xE0;                     /* tunnel, cliff, drop-off on that side */
    if ((r->state | f->state) & blocked) return;
    if (!plain_sprite(t)) return;
    u32 h = unit_hash(r->unit);
    int n = 1 + (int)(h & 1);
    for (int i = n - 1; i >= 0; i--) {                    /* outermost (farthest) first */
        u32 hi = unit_hash(r->unit * 4 + i + 1);
        /* its kind: the placed one's, or that of one of the four units before it if that is a plain sprite */
        s8 te = t, oe;
        int back = 1 + (int)((hi >> 4) & 3);
        if (hi & 0x100) {
            bool ok;
            if (S->front) {
                u16 k = (u16)((u8)(r->phase - back) & 0x7F);
                te = DSC((u16)(DS_dat_scenery_type + k));
                ok = true;
            } else {
                ok = enh_scenery_at(r->unit - back, &te, &oe);   /* as the ring held it (history) */
            }
            if (!ok || !plain_sprite(te)) te = t;
        }
        u16 base = plain_sprite(te);
        if (!base) continue;
        double dj = EXTRA_DEPTH * (double)((hi >> 8) & 0xFF) / 255.0;
        float W = lerpf(r->W, f->W, (float)dj), y = lerpf(r->y, f->y, (float)dj);
        float edge = right ? lerpf(r->R, f->R, (float)dj) : lerpf(r->L, f->L, (float)dj);
        int d = EXTRA_OUT + EXTRA_STEP * i + (int)((hi >> 16) % 3);
        s16 oe2 = (s16)(right ? off + d : off - d);
        float x = oe2 * W / 8 + edge;
        if (right) {                                          /* not beyond the far-right band */
            float band = lerpf(r->band, f->band, (float)dj);
            if (band < 2000 && x + W / 2 > band) continue;
        }
        u16 di = (u16)(base + 4 * s5_cur);
        float ks = SCALE5(base, W);
        and_h(di, x, y, ks);
        or_h((u16)(di + 0x140), x, y, ks);
    }
}

static void scenery(int j, double zlim_near)                               /* §4.10 */
{
    const EnhRow *r = &S->rows[j];
    u16 k = (u16)(r->phase & 0x7F);
    s8 t, soff;
    if (S->front) {
        t = DSC((u16)(DS_dat_scenery_type + k));
        soff = DSC((u16)(DS_dat_scenery_offset + k));
    } else if (!enh_scenery_at(r->unit, &t, &soff)) {
        return;                                            /* not passed since the stage (life) started */
    }
    if (t < 0) return;
    if ((u8)t < 0x50) {
        u16 base = (u16)(DS_scenery_handles + ((u16)(u8)t << 2));
        u16 di = (u16)(base + 4 * s5_cur);
        if (DSW((u16)(di + 2)) != 0) {
            s16 off = soff;
            off = (s16)(off >= 0 ? off + 2 : off - 2);
            float x = off * r->W / 8;
            x += x > 0 ? r->R : r->L;
            if (group_cut_off(base, &fam5)) {
                /* opaque where the original draws it, fading in over the 5 units beyond */
                double a = (zlim_near + 5 - r->z) / 5;
                if (a <= 0) return;
                if (a < cur_alpha) cur_alpha = (float)a;
            }
            scenery_extras(j, t, off);
            float ks = SCALE5(base, r->W);
            and_h(di, x, r->y, ks);
            or_h((u16)(di + 0x140), x, r->y, ks);
            return;
        }
    }
    if ((u8)t < 0x1E) return;
    text_sign(j, t, soff);
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
    if (dx >= 0 && dx < V_W) { and_h(bx, dx, cy, k); or_h((u16)(bx + 0x10), dx, cy, k); }
    if (bp >= 0 && bp < V_W) { and_h(bx, bp, cy, k); or_h((u16)(bx + 0x10), bp, cy, k); }
}

/* ------------------------------------------------------------------------------------------------ */
/* cars                                                                                             */

typedef struct { const EnhCar *c; double jf; } CarRef;   /* jf: fractional row */
static CarRef car_list[ENH_MAX_CARS];
static int car_n, car_next;

/* Car size variants (as in Test Drive Enhanced): the original picks the variant from its carscale table by
 * row, and beyond its rows it would be the smallest. Here the most detailed variant is used that is still
 * drawn at CAR_LOD_MIN of its own size or larger (the group's continuous height, group_scale), so cars are
 * mostly scaled down instead of up and the smaller variants give way to larger ones further away; the
 * smallest variant, whose heavy outline stands out, is never used. */
#define CAR_LOD_MIN 0.5

static int car_variant(u16 base, int stride, double W)
{
    const Family *f = &famcar;
    int lo = -1, second = -1;                               /* the smallest and the next variant in use */
    for (int k = 0; k < f->n; k++) {
        if (!f->ok[k] || !enh_sprite(hnd_at((u16)(base + stride * k)))) continue;
        if (lo < 0) lo = k;
        else if (second < 0) second = k;
    }
    if (lo < 0) return 0;
    int least = second >= 0 ? second : lo;
    for (int k = f->n - 1; k > least; k--) {
        if (!f->ok[k] || !enh_sprite(hnd_at((u16)(base + stride * k)))) continue;
        if (group_scale(base, stride, k, f, W) >= CAR_LOD_MIN) return k;
    }
    return least;
}

/* road values at fractional row jf (row j = unit car_unit + j) */
static bool road_at(double jf, double *z, double *X, double *H, double *Rw)
{
    if (jf < -1 || jf >= nrows) return false;
    int j = jf < 0 ? 0 : (int)jf;                          /* before row 0: extrapolated from rows 0 and 1 */
    double t = jf - j;
    const EnhRow *a = &S->rows[j], *b = &S->rows[j + 1];
    *z = a->z + t;
    *X = a->X + (b->X - a->X) * t;
    *H = a->H + (b->H - a->H) * t;
    /* the right edge including widening, interpolated in world units */
    double ra = (a->R - S->centre) * a->z, rb = (b->R - S->centre) * b->z;
    *Rw = ra + (rb - ra) * t;
    return true;
}

/* front view only: call after enh_scene_build */
double enh_scene_screen_x(double s_unit, double lat)
{
    double z, X, H, Rw;
    if (!road_at(s_unit - car_unit, &z, &X, &H, &Rw)) return NAN;
    return S->centre + (X + lat) * KX / z;
}

static void draw_car(const EnhCar *c, double jf, double zlim)
{
    double z, X, H, Rw;
    if (!road_at(jf, &z, &X, &H, &Rw)) return;
    bool front = S->front;
    float save = cur_cy1;
    int jn = (int)ceil(jf) - 1;                            /* rows nearer than the car: 0..jn */
    if (jn < 0) jn = 0;
    float clip = S->rows[jn].clip;
    float yroad = (float)(S->horizon + H * KY / z);
    if (yroad < clip) clip = yroad;
    cur_cy1 = clip;
    cur_alpha = fade(z, zlim);
    float W = (float)(KW / z);
    int s;
    float y = yroad - 1;
    float x = (float)(S->centre + (X + c->lat * S->lat_k) * KX / z);
    switch (c->kind) {
    case ENH_CAR_TRAFFIC: {
        u16 bx = (u16)(c->type - 1);
        if (!front) bx ^= 4;                               /* rear views in the mirror */
        u16 e = (u16)((((bx & 3) << 4) + ((bx & 4) << 1)) << 1);
        u16 base = (u16)(DS_traffic1_handles + (e << 2));
        s = car_variant(base, 4, W);
        float k = group_scale(base, 4, s, &famcar, W);
        and_h((u16)(base + 4 * s), x, y, k);
        or_h((u16)(base + 0x20 + 4 * s), x, y, k);
        break;
    }
    case ENH_CAR_OPP: {
        u16 base = (u16)((front ? DS_opp_road_handles : DS_opp_front_handles)   /* rc?? / fc?? */
                         + (DSB(DS_opp_crash_timer) != 0 ? 0x40 : 0));
        s = car_variant((u16)(base + 0x20), 4, W);
        float k = group_scale((u16)(base + 0x20), 4, s, &famcar, W);
        and_h((u16)(base + 0x20 + 4 * s), x, y, k);
        or_h((u16)(base + 4 * s), x, y, k);
        if (front && DSB(DS_opp_braking) != 0) xor_h((u16)(DS_opp_road_handles + 0x80 + 4 * s), x, y, k);
        break;
    }
    case ENH_CAR_COP: {
        u16 base = (u16)(DS_cop_car_handles + (front ? 0x40 : 0));   /* COP rc?M / rcr?, mirror fc?M / fcr? */
        s = car_variant(base, 4, W);
        float k = group_scale(base, 4, s, &famcar, W);
        and_h((u16)(base + 4 * s), x, y, k);
        or_h((u16)(base + 0x20 + 4 * s), x, y, k);
        if (front && DSB(DS_cop_braking) != 0) xor_h((u16)(DS_cop_extra_handles + 0x20 + 4 * s), x, y, k);
        if (DSW(DS_stage_time) & 1) xor_h((u16)(DS_cop_extra_handles + 4 * s), x, y, k);
        break;
    }
    default: {                                             /* parked police car at the right edge */
        u16 fr = (u16)(DSW(DS_sim_tick10) & 0x0C);
        u16 base = (u16)(DS_cop_extra_handles + 0x40 + fr);
        s = car_variant((u16)(base + 0x80), 16, W);
        float k = group_scale((u16)(base + 0x80), 16, s, &famcar, W);
        float xr = (float)(S->centre + Rw / z);
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
        const CarRef *c = &car_list[car_next];
        if (c->jf <= j - 1) break;
        car_next++;
        if (c->jf <= j) draw_car(c->c, c->jf, zlim);
    }
}

static int car_cmp(const void *pa, const void *pb)
{
    const CarRef *a = pa, *b = pb;
    if (a->jf != b->jf) return a->jf > b->jf ? -1 : 1;     /* far first */
    return a->c->order - b->c->order;
}

/* The cars of this view, far first. A car at d = s_car - s units ahead of the player is in the front view
 * for d >= 1 and in the mirror for d < 1 (the original's snapshot distances); the parked police car is
 * drawn one row nearer than its position. */
static void sort_cars(const EnhView *v, const EnhCar *cars, int ncars)
{
    car_n = 0;
    for (int i = 0; i < ncars && car_n < ENH_MAX_CARS; i++) {
        double se = cars[i].s;
        if (cars[i].kind == ENH_CAR_PARKED) se += S->front ? -1 : 1;
        double d = se - v->s, jf = S->front ? se - car_unit : car_unit + 1 - se;
        if (S->front ? d < 1 : d >= 1) continue;
        if (jf < -1 || jf >= nrows) continue;
        car_list[car_n++] = (CarRef){ &cars[i], jf };
    }
    qsort(car_list, (size_t)car_n, sizeof *car_list, car_cmp);
    car_next = 0;
}

/* ------------------------------------------------------------------------------------------------ */

/* clipping of row j's objects: nearer rows (crests), the tunnel ceiling and the far end of the nearest tunnel */
static void row_clips(int j)
{
    set_ceiling_clip(j);
    cur_cy1 = S->rows[j].clip;
    if (!S->style && (S->r0_any & 0x80) && S->tunnel_out_found && j > S->tunnel_out_row) {
        cur_cx0 = S->tunnel_out_l;
        cur_cx1 = S->tunnel_out_r;
    } else {
        cur_cx0 = 0;
        cur_cx1 = V_W;
    }
}

static const u8 CLIFF_BIT[2] = { 0x08, 0x40 };       /* right, left */

/* The rock faces of one side. Within the original's rows they are drawn together where the original draws
 * its cliff fill: at the row whose edge reaches farthest into the view (its cut row; everything farther is
 * behind the rock, everything nearer - cars, poles, the cliff decorations - is drawn over it). Beyond them
 * each pair is drawn at its own row. */
static int face_row[2], near_face[2];

static void faces_setup(void)
{
    for (int side = 0; side < 2; side++) {
        face_row[side] = near_face[side] = -1;
        float best = 0;
        int lim = CUT_ROWS < nrows ? CUT_ROWS : nrows;
        for (int j = nrows; j >= 1; j--) {
            u8 st = S->rows[j].state;
            if ((st & 0x80) || !(st & CLIFF_BIT[side])) continue;
            near_face[side] = j;
            if (j > lim) continue;
            float e = side ? S->rows[j].ol : -S->rows[j].or_;
            if (face_row[side] < 0 || e > best) { best = e; face_row[side] = j; }
        }
    }
}

static void faces_at(int j)
{
    for (int side = 0; side < 2; side++) {
        int from = j, to = j;
        if (j <= face_row[side]) {                         /* within the original's rows */
            if (j != face_row[side]) continue;
            to = 1;
        }
        for (int k = from; k >= to; k--) {
            u8 st = S->rows[k].state;
            if ((st & 0x80) || !(st & CLIFF_BIT[side])) continue;
            row_clips(k);
            cur_alpha = fade(S->rows[k].z, zlim_rock);        /* far rock fades out into the view's end */
            cliff_face(k, side == 1, k == near_face[side] && k <= CUT_ROWS);
            cur_alpha = 1;
        }
    }
    row_clips(j);
}

static void objects(double zlim_obj, double zlim_scn)
{
    zlim_rock = zlim_obj;
    faces_setup();
    for (int j = nrows; j >= 0; j--) {
        const EnhRow *r = &S->rows[j];
        j_cur = j;
        st_cur = r->state;
        float W = r->W;
        W_cur = W;
        int os = (int)W >> 3;
        if (os >= 0x1F) os = 0x1F;
        os_cur = W / 8;
        s5_cur = (os >= 16 ? 16 : os) / 4;
        s4_cur = (os >> 1) / 4;
        cur_alpha = 1;
        line_w = 1;
        row_clips(j);

        /* 1. road markings of the scanlines between this row and the nearer one */
        if (j >= 1) {
            EnhCmd *c = cmd(CMD_MARK);
            if (c) c->a = j;
        }

        /* drop-off sides below the road edge (enh_raster.c do_drop), over the whole height of the view */
        if (j >= 1 && !(st_cur & 0x80) && (st_cur & 0x24)) {
            float save = cur_cy1;
            cur_cy1 = V_H;
            for (int side = 0; side < 2; side++) {
                if (!(st_cur & (side ? 0x20 : 0x04))) continue;
                EnhCmd *c = cmd(CMD_DROP);
                if (!c) continue;
                c->a = j;
                c->op = (u8)side;
            }
            cur_cy1 = save;
        }

        /* 2. left cliff, 3. right cliff: rock faces instead of the original's cut-line fill and edge sprite */
        if (j >= 1) faces_at(j);
        if (st_cur & 0x40) {
            if (S->front && !(st_cur & 0x80) && j <= 23 && j < face_row[1]) cliff_deco(j, true);
        }
        if (st_cur & 0x08) {
            if (S->front && !(st_cur & 0x80) && j <= 23 && j < face_row[0]) cliff_deco(j, false);
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

        /* 7. scenery (the ring holds ENH_SCENERY_AHEAD units ahead of the last simulation step; the mirror
         * reads the units behind from their history) */
        int ahead = r->unit - step_unit - 1;
        if ((!S->front || ahead <= ENH_SCENERY_AHEAD) && z < zlim_scn) {
            cur_alpha = fade(z, zlim_scn);
            scenery(j, S->scenery_rows + S->depth0);
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

static void build(EnhScene *sc, bool front, const EnhView *v, const EnhCar *cars, int ncars)
{
    S = sc;
    view_setup(sc, front);
    Families *fm = &fams[front ? 0 : 1];
    if (!fm->init) {
        family_init(&fm->f4, 4, variant4);
        family_init(&fm->f5, 5, variant5);
        fm->init = true;
    }
    family_init(&fm->car, 8, variantcar);
    S->ncmds = 0;
    S->col_left = (u8)(DSW(DS_scene_words) & 15);
    S->col_right = (u8)(DSW(DS_col_right) & 15);
    S->col_shoulder = (u8)(DSW(DS_col_shoulder) & 15);
    S->col_sky = (u8)(DSW(DS_col_sky) & 15);
    S->col_far = (u8)(DSW(DS_col_far) & 15);
    enh_colours_setup(S->col_left, S->col_right, S->col_shoulder, S->col_sky);
    project(v);
    /* road position at depth z (front row j: unit car_unit + j at depth j + 3 - frac; mirror row j: unit
     * car_unit + 1 - j at depth j + 5 + frac) */
    S->u0 = front ? v->s - 3 : v->s + 6;
    S->uk = front ? 1 : -1;
    {
        double hs = v->heading - v->yaw * 8.0 / 256.0;       /* the mountains' scroll */
        S->valley_shift = front ? hs : -hs / 2;
    }
    cut_lines();
    ground_pairs();

    cur_cx0 = 0;
    cur_cx1 = V_W;
    sky(v);
    cur_cy0 = 0;
    cur_cy1 = V_H;
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

    sort_cars(v, cars, ncars);

    double zlim_obj = nrows + S->depth0 - 2;
    double zlim_scn = S->front && ENH_SCENERY_AHEAD < zlim_obj ? ENH_SCENERY_AHEAD : zlim_obj;
    objects(zlim_obj, zlim_scn);
}

void enh_scene_build(const EnhView *v, const EnhCar *cars, int ncars)
{
    build(&enh_sc, true, v, cars, ncars);

    /* Falling off the road (draw_front, scene_render.md §4.7): the view is scrolled up by fall_scroll
     * (once it is 92 or more, only the fills remain), and below it a drop shows sky on the open side and
     * the cliff on the other, the water mode fills with colour 9. */
    S->yoff = 0;
    if (v->fall_mode != 0 && v->fall_v > 0) {
        float fv = (float)v->fall_v;
        if (fv >= V_H) S->ncmds = 0;
        S->yoff = fv;
        cur_cy0 = 0;
        cur_cy1 = V_H;
        cur_cx0 = 0;
        cur_cx1 = V_W;
        cur_alpha = 1;
        int first = S->ncmds;
        float cy = V_H - fv;
        if (v->fall_mode == 4) {
            fill(0, cy, V_W, 180, 9);
        } else {
            bool left = v->fall_mode == 1;
            /* the original's cut x of the drawn view (the fall view keeps its geometry exactly) */
            s16 ox = DSS(left ? DS_left_sky_x : DS_right_sky_x);
            float bx = ox < 0 ? 0 : ox > V_W ? V_W : ox;
            u16 cl = left ? S->col_sky : 6, cr = left ? 6 : S->col_sky;
            fill(0, cy + 180, V_W, 100, 6);
            fill(bx, cy, V_W - bx, 180, cr);
            fill(0, cy, bx, 180, cl);
        }
        for (int k = first; k < S->ncmds; k++) S->cmds[k].noshift = 1;
    }
}

/* The mirror (draw_mirror, scene_render.md §4.7, §4.11). Falling: the original stops drawing the mirror
 * view; in the water it scrolls its last image up (by fall_scroll / 8 per drawn frame, mirror_fall is that
 * sum) and fills colour 9 below, otherwise the mirror shows sky above and the cliff below a line fall_scroll
 * / 8 under its horizon. */
void enh_mirror_build(const EnhView *v, const EnhCar *cars, int ncars, double mirror_fall)
{
    build(&enh_mc, false, v, cars, ncars);
    S->yoff = 0;
    if (v->fall_mode == 0 || v->fall_v <= 0) return;
    cur_cy0 = 0;
    cur_cy1 = V_H;
    cur_cx0 = 0;
    cur_cx1 = V_W;
    cur_alpha = 1;
    int first;
    if (v->fall_mode == 4) {
        float sh = (float)mirror_fall;
        if (sh >= V_H) S->ncmds = 0;
        S->yoff = sh;
        first = S->ncmds;
        fill(0, V_H - sh, V_W, 180, 9);
    } else {
        S->ncmds = 0;
        first = 0;
        float ax = (float)(v->fall_v / 8) + DSS((u16)(DS_top_sy + 0x195C));
        fill(0, ax, V_W, V_H - ax, 6);
        fill(0, 0, V_W, ax, S->col_sky);
    }
    for (int k = first; k < S->ncmds; k++) S->cmds[k].noshift = 1;
}
