/* Enhanced renderer: hooks, simulation snapshots and extrapolation, frame orchestration, coverage of the
 * EGA overlays and the screen overlay (ENHANCED.md). Not bound by the faithful-engine rules.
 *
 * The faithful stage loop keeps drawing its own frame; enh_frame() renders the front view again from the
 * simulation state extrapolated to the current time and lays it over rows 19..110 of the EGA frame, except
 * where the original drew something over the road view (mirror, mirror frame, ticket) or on the screen
 * after presenting it (messages, windscreen cracks, GAME OVER). */
#include "enh_internal.h"
#include "../host.h"
#include "../game/flow.h"
#include "../game/scene.h"
#include "../platform/gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROAD0 0x3B51
#define STEP_NS 100000000.0                 /* one 10 Hz simulation step */

int enh_rows_setting = ENH_DEFAULT_ROWS;
static bool enabled;                        /* false: --classic */
static bool active;                         /* overlay shows a rendered frame */
static bool window_on;                      /* the road window is replaced (not in the fall view) */
static bool dirty;

/* ------------------------------------------------------------------------------------------------ */
/* simulation snapshots                                                                             */

#define MAX_TRAFFIC 50

typedef struct {
    bool valid;
    uint64_t t;
    u16 pos, heading, cloud, counter;
    u8 sub, start_flags;
    s16 lat, yaw;
    u16 speed, opp_speed, cop_speed;
    u16 opp_pos, cop_pos;
    u8 opp_sub, cop_sub;
    s16 opp_lat, cop_lat;
    int n[2];
    u16 type[2][MAX_TRAFFIC], cpos[2][MAX_TRAFFIC];
    u8 csub[2][MAX_TRAFFIC];
    s16 cx[2][MAX_TRAFFIC];
} Snap;

static Snap prev, last;

static void take(Snap *s, uint64_t t)
{
    s->valid = true;
    s->t = t;
    s->pos = DSW(DS_player_pos);
    s->sub = DSB(DS_player_pos_lo);
    s->lat = DSS(DS_player_lateral);
    s->yaw = DSS(DS_view_yaw);
    s->heading = DSW(DS_heading);
    s->cloud = DSW(DS_cloud_scroll);
    s->counter = DSW(DS_ring_counter);
    s->start_flags = DSB(DS_start_flags);
    s->speed = DSW(DS_speed);
    s->opp_speed = DSW(DS_opp_speed);
    s->cop_speed = DSW(DS_cop_speed);
    s->opp_pos = DSW(DS_opp_pos);
    s->opp_sub = DSB(DS_opp_pos_lo);
    s->opp_lat = DSS(DS_opp_lateral);
    s->cop_pos = DSW(DS_cop_pos);
    s->cop_sub = DSB(DS_cop_pos_lo);
    s->cop_lat = DSS(DS_cop_lateral);
    for (int l = 0; l < 2; l++) {
        u16 base = l == 0 ? DS_oncoming : DS_same_dir;
        int n = (l == 0 ? DSW(DS_oncoming_count8) : DSW(DS_same_count8)) / 8;
        if (n < 1) n = 1;                                 /* snapshot_front reads the first entry anyway */
        if (n > MAX_TRAFFIC) n = MAX_TRAFFIC;
        s->n[l] = n;
        for (int i = 0; i < n; i++) {
            u16 e = (u16)(base + 8 * i);
            s->type[l][i] = DSW(e);
            s->cpos[l][i] = DSW((u16)(e + 2));
            s->csub[l][i] = DSB((u16)(e + 4));
            s->cx[l][i] = DSS((u16)(e + 6));
        }
    }
}

void enh_sim_step(void)
{
    if (!enabled) return;
    uint64_t t = host_tick_ns();
    if (last.valid) prev = last;
    take(&last, t);
    if (!prev.valid) prev = last;
}

static void snap_reset(void)
{
    prev.valid = last.valid = false;
}

/* ------------------------------------------------------------------------------------------------ */
/* view state for this frame                                                                        */

static double pos_of(u16 pos, u8 sub) { return (double)(s32)(pos - ROAD0) + sub / 256.0; }

static const u8 *rec_of(int u)
{
    long a = (long)ROAD0 + u;
    u8 b = (a >= ROAD0 && a < 0x52C8) ? DSB((u16)a) : 0;
    return mp(DGROUP, (u16)(DS_road_records + (b & 0x7F) * 4));
}

static EnhView view;
static const char *compare_dir;             /* developer aid, see enh_init */
static EnhCar cars[ENH_MAX_CARS];
static int ncars;

/* Advance over the next step. A driver moves speed_hi * 3 sub-units per step with the speed of that step;
 * the speed is extrapolated too, so a steadily accelerating car does not jump at each step. Falls back to
 * the last step's advance when that was not a normal speed step (standing, crash, restart). */
static double step_advance(double ds_last, u16 v_last, u16 v_prev)
{
    double by_speed = (v_last >> 8) * 3 / 256.0;
    if (fabs(ds_last - by_speed) > 0.02) return ds_last;
    s32 v = (s32)v_last + ((s32)v_last - (s32)v_prev);
    if (v < 0) v = 0;
    if (v > 0xFFFF) v = 0xFFFF;
    return (v >> 8) * 3 / 256.0;
}

static double lerp_d(double a, double d, double alpha, double limit)
{
    return fabs(d) > limit ? a : a + alpha * d;
}

/* traffic / AI car position extrapolated like the player's, relative to the player (wrapped) */
static bool car_rel(u16 lp, u8 ls, u16 pp, u8 ps, bool have_prev, double alpha, double s, double len, double *d,
                    int speed_dir, u16 v_last, u16 v_prev)
{
    double t = pos_of(lp, ls);
    if (have_prev) {
        double dt = t - pos_of(pp, ps);
        if (dt > len / 2) dt -= len;
        if (dt < -len / 2) dt += len;
        if (speed_dir != 0 && dt >= 0 && dt <= 4) dt = step_advance(dt, v_last, v_prev);
        if (fabs(dt) <= 4) t += alpha * dt;
    }
    double r = t - s;
    if (len > 0) {
        r = fmod(r, len);
        if (r < 0) r += len;
    }
    *d = r;
    return true;
}

static void add_car(double s, double lat, int kind, u16 type)
{
    if (ncars == ENH_MAX_CARS) return;
    cars[ncars] = (EnhCar){ s, lat, kind, type, ncars };
    ncars++;
}

static void compute_view(void)
{
    uint64_t now = host_time_ns();
    if (!last.valid) {
        take(&last, now);
        prev = last;
    }
    bool frozen = DSB(DS_run_state) != 0 || DSB(DS_fall_mode) != 0;
    double alpha = (double)(now > last.t ? now - last.t : 0) / STEP_NS;
    if (alpha > 1) alpha = 1;
    if (frozen || compare_dir) alpha = 0;

    /* player */
    double sl = pos_of(last.pos, last.sub), ds = sl - pos_of(prev.pos, prev.sub);
    if (ds < 0 || ds > 8) ds = 0;
    else ds = step_advance(ds, last.speed, prev.speed);
    double s = sl + alpha * ds;
    if (compare_dir) s = floor(s);                        /* the original ignores the sub-unit position */
    view.s = s;
    view.lat = lerp_d(last.lat, (double)last.lat - prev.lat, alpha, 300);
    view.yaw = lerp_d(last.yaw, (double)last.yaw - prev.yaw, alpha, 0x800);
    view.pos = last.pos;
    view.counter = last.counter;
    view.start_flags = last.start_flags;
    view.frozen = frozen;

    /* mountain and cloud scroll: the curve of every unit is added as the car moves through it */
    int P = (int)(s32)(last.pos - ROAD0), V = (int)floor(s);
    double heading = last.heading, cloud = last.cloud;
    for (int u = P + 1; u <= V + 1; u++) {
        s16 h = (s16)((s8)rec_of(u)[1] >> 1);
        double w = u <= V ? 1.0 : s - V;
        heading += w * h;
        cloud += w * (s16)((h >> 2) + h);
    }
    view.heading = heading;
    view.cloud = cloud;

    /* cars */
    ncars = 0;
    double len = DSW(DS_dat_road_units);
    int nr = enh_rows_setting;
    double d;
    for (int l = 1; l >= 0; l--) {                        /* the original's list order, reversed */
        bool same = last.n[l] == prev.n[l];
        for (int i = last.n[l] - 1; i >= 0; i--) {
            bool hp = same && prev.type[l][i] == last.type[l][i];
            car_rel(last.cpos[l][i], last.csub[l][i], prev.cpos[l][i], prev.csub[l][i], hp, alpha, s, len, &d, 0, 0, 0);
            if (d < 1 || d > nr + 1) continue;
            double x = hp ? lerp_d(last.cx[l][i], (double)last.cx[l][i] - prev.cx[l][i], alpha, 100)
                          : last.cx[l][i];
            add_car(s + d, x, ENH_CAR_TRAFFIC, last.type[l][i]);
        }
    }
    if (DSB(DS_opponent_enabled) != 0) {
        car_rel(last.opp_pos, last.opp_sub, prev.opp_pos, prev.opp_sub, prev.opp_pos != 0, alpha, s, len, &d, 1,
                last.opp_speed, prev.opp_speed);
        if (d >= 1 && d <= nr + 1)
            add_car(s + d, lerp_d(last.opp_lat, (double)last.opp_lat - prev.opp_lat, alpha, 200), ENH_CAR_OPP, 0);
    }
    bool cop_moving = DSB(DS_cop_active) != 0, cop_parked = DSB(DS_cop_state) >= 7;
    if (cop_moving || cop_parked) {
        bool hp = prev.cop_pos != 0 && last.cop_pos != 0;
        car_rel(last.cop_pos, last.cop_sub, prev.cop_pos, prev.cop_sub, hp, alpha, s, len, &d, 1, last.cop_speed,
                prev.cop_speed);
        double x = hp ? lerp_d(last.cop_lat, (double)last.cop_lat - prev.cop_lat, alpha, 200) : last.cop_lat;
        if (cop_moving && d >= 1 && d <= nr + 1) add_car(s + d, x, ENH_CAR_COP, 0);
        if (cop_parked && d >= 2 && d <= nr + 2) add_car(s + d - 1, 0, ENH_CAR_PARKED, 0);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* coverage: pixels of the road window that keep the EGA image                                      */

static u8 cover[VIEW_W * VIEW_H];
static u8 main_snap[4][40 * VIEW_H];
static u8 vram_snap[4][40 * VIEW_H];

static bool main_planes(const u8 *pl[4], u16 *rows)
{
    FarPtr desc = ds_far(DS_drive_buf_desc);
    if (desc.seg == 0) return false;
    u16 rowtab = DSW(DS_main_rowtab);
    for (int k = 0; k < 4; k++) {
        u16 seg = rd16(desc.seg, (u16)(desc.off + 2 + 2 * k));
        if (seg == 0) return false;
        pl[k] = mp(seg, 0);
    }
    for (int y = 0; y < VIEW_H; y++) rows[y] = CSW((u16)(rowtab + 2 * y));
    return true;
}

void enh_before_overlays(void)
{
    if (!enabled) return;
    const u8 *pl[4];
    u16 rows[VIEW_H];
    if (!main_planes(pl, rows)) return;
    for (int k = 0; k < 4; k++)
        for (int y = 0; y < VIEW_H; y++) memcpy(main_snap[k] + 40 * y, pl[k] + rows[y], 40);
}

static void cover_rect(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= VIEW_H) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < VIEW_W) cover[y * VIEW_W + x] = 1;
    }
}

static void update_cover(void)
{
    memset(cover, 0, sizeof cover);
    const u8 *pl[4];
    u16 rows[VIEW_H];
    if (main_planes(pl, rows)) {
        for (int y = 0; y < VIEW_H; y++)
            for (int bx = 0; bx < 40; bx++) {
                u8 ch = 0;
                for (int k = 0; k < 4; k++) ch |= (u8)(pl[k][(u16)(rows[y] + bx)] ^ main_snap[k][40 * y + bx]);
                if (!ch) continue;
                for (int b = 0; b < 8; b++)
                    if (ch & (0x80 >> b)) cover[y * VIEW_W + bx * 8 + b] = 1;
            }
    }
    /* the mirror, its frame and the ticket, whatever colours they happen to have */
    FarPtr ms = ds_far(DS_mirror_sprite);
    if (ms.seg != 0) cover_rect((s16)rd16(ms.seg, (u16)(ms.off + 8)), (s16)rd16(ms.seg, (u16)(ms.off + 10)), 80, 17);
    const EnhSprite *m = enh_sprite(ds_far(DASH_H(DASH_mirr)));
    if (m) enh_cover_sprite(cover, VIEW_W, VIEW_H, m, m->ox, m->oy, EOP_AND);
    u8 cs = DSB(DS_cop_state);
    if (cs >= 4 && cs <= 6) {
        const EnhSprite *t = enh_sprite(ds_far(ROAD_H(ROAD_tick)));
        if (t) cover_rect(t->ox, t->oy, t->w, t->h);
        const EnhSprite *dg = enh_sprite(ds_far(ROAD_H(ROAD_dgt0)));
        if (dg) cover_rect(0x37, 0x13, 0x41 - 0x37 + dg->w, dg->h);
    }
    for (int k = 0; k < 4; k++) memcpy(vram_snap[k], gfx_vram_plane(k) + VIEW_Y0 * 40, sizeof vram_snap[k]);
}

/* ------------------------------------------------------------------------------------------------ */
/* overlay                                                                                          */

static bool ov_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}

static uint64_t ov_ns;

static void ov_draw(u32 *px, int k)
{
    if (!active || !window_on || k != enh_scale || !enh_out) return;
    uint64_t t0 = host_time_ns();
    if (enh_palette_key() != enh_resolved_palette_key()) enh_resolve();
    int ow = VIEW_W * k;
    for (int y = 0; y < VIEW_H; y++) {
        const u8 *p[4], *s[4];
        for (int n = 0; n < 4; n++) {
            p[n] = gfx_vram_plane(n) + (VIEW_Y0 + y) * 40;
            s[n] = vram_snap[n] + y * 40;
        }
        const u8 *cv = cover + y * VIEW_W;
        for (int bx = 0; bx < 40; bx++) {
            u8 changed = (u8)((p[0][bx] ^ s[0][bx]) | (p[1][bx] ^ s[1][bx]) | (p[2][bx] ^ s[2][bx])
                              | (p[3][bx] ^ s[3][bx]));
            for (int b = 0; b < 8; b++) {
                int x = bx * 8 + b;
                if (cv[x] || (changed & (0x80 >> b))) continue;
                for (int j = 0; j < k; j++) {
                    u32 *d = px + (size_t)((VIEW_Y0 + y) * k + j) * ow + (size_t)x * k;
                    const u32 *src = enh_out + (size_t)(y * k + j) * enh_ow + (size_t)x * k;
                    for (int i = 0; i < k; i++) d[i] = src[i];
                }
            }
        }
    }
    ov_ns += host_time_ns() - t0;
}

/* ------------------------------------------------------------------------------------------------ */
/* developer aids                                                                                   */

static bool debug_on, stats_on;
static FILE *trace;                         /* TD2_ENH_TRACE=file: per-frame view values */

/* TD2_ENH_COMPARE_DIR: every 2 s, cmpNNNN.bmp with the enhanced road window (no extrapolation, whole
 * road units) above the original's window at the same moment, scaled up. */
static void compare_dump(void)
{
    static uint64_t last_ns;
    static int n;
    uint64_t now = host_time_ns();
    if (n && now - last_ns < 2000000000ull) return;
    last_ns = now;
    int k = enh_scale, w = enh_ow, h = 2 * enh_oh;
    char path[512];
    snprintf(path, sizeof path, "%s/cmp%04d.bmp", compare_dir, n++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int stride = (w * 3 + 3) & ~3;
    u32 size = 54 + (u32)(stride * h);
    u8 hd[54] = { 'B', 'M' };
    hd[2] = (u8)size; hd[3] = (u8)(size >> 8); hd[4] = (u8)(size >> 16); hd[5] = (u8)(size >> 24);
    hd[10] = 54; hd[14] = 40;
    hd[18] = (u8)w; hd[19] = (u8)(w >> 8); hd[22] = (u8)h; hd[23] = (u8)(h >> 8);
    hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    u8 *line = calloc((size_t)stride, 1);
    for (int y = h - 1; y >= 0 && line; y--) {
        for (int x = 0; x < w; x++) {
            u32 c;
            if (y < enh_oh) {
                c = enh_out[(size_t)y * enh_ow + x];
            } else {
                int oy = (y - enh_oh) / k + VIEW_Y0, ox = x / k;
                int idx = 0;
                for (int p = 0; p < 4; p++)
                    idx |= ((gfx_vram_plane(p)[oy * 40 + (ox >> 3)] >> (7 - (ox & 7))) & 1) << p;
                c = gfx_palette_rgb((u8)idx);
            }
            line[x * 3] = (u8)c;
            line[x * 3 + 1] = (u8)(c >> 8);
            line[x * 3 + 2] = (u8)(c >> 16);
        }
        fwrite(line, 1, (size_t)stride, f);
    }
    free(line);
    fclose(f);
    snprintf(path, sizeof path, "%s/cmp%04d.txt", compare_dir, n - 1);
    f = fopen(path, "w");
    if (!f) return;
    const EnhScene *S = &enh_sc;
    fprintf(f, "s %.3f top %.2f/%d any %02X state %02X sky %02X start %02X style %d near %d out_found %d\n",
            view.s, S->top_sy, S->top_row, S->r0_any, S->r0_state, S->sky_state, S->start_flags, S->style,
            S->tunnel_nearest, S->tunnel_out_found);
    fprintf(f, "cut L %.1f/%d/%02X R %.1f/%d/%02X sky L %.1f/%d/%.1f R %.1f/%d/%.1f\n", S->left_cut_x,
            S->left_cut_row, S->left_cut_state, S->right_cut_x, S->right_cut_row, S->right_cut_state,
            S->left_sky_x, S->left_sky_row, S->left_sky_y, S->right_sky_x, S->right_sky_row, S->right_sky_y);
    fprintf(f, "tunnel in %d sy %.1f top %.1f l %.1f r %.1f out %d sy %.1f top %.1f l %.1f r %.1f ceil %.1f/%d extra %d\n",
            S->tunnel_in_row, S->tunnel_in_sy, S->tunnel_in_top, S->tunnel_in_l, S->tunnel_in_r, S->tunnel_out_row,
            S->tunnel_out_sy, S->tunnel_out_top, S->tunnel_out_l, S->tunnel_out_r, S->tunnel_ceiling,
            S->tunnel_ceiling_row, S->nextra);
    for (int j = 0; j <= S->nrows; j++) {
        const EnhRow *r = &S->rows[j];
        fprintf(f, "row %3d u %5d z %6.2f y %6.2f cx %7.1f L %7.1f R %7.1f ol %7.1f or %7.1f W %6.1f clip %5.1f fl %02X st %02X ob %02X ph %02X\n",
                j, r->unit, r->z, r->y, r->cx, r->L, r->R, r->ol, r->or_, r->W, r->clip, r->flags, r->state, r->obj, r->phase);
    }
    static const char *names[] = { "FILL", "SPRITE", "LINE", "GROUND", "WALLS", "BAND", "MARK" };
    for (int k = 0; k < S->ncmds; k++) {
        const EnhCmd *c = &S->cmds[k];
        if (c->type == CMD_MARK) continue;
        fprintf(f, "%s c%d op%d clip y %.1f..%.1f x %.1f..%.1f  %.1f %.1f %.1f %.1f a %.2f\n", names[c->type], c->colour, c->op,
                c->cy0, c->cy1, c->cx0, c->cx1, c->x0, c->y0, c->x1, c->y1, c->alpha);
    }
    fclose(f);
}
static uint64_t stat_ns, stat_max_ns;
static int stat_frames, stat_cmds;

static void debug_sizes(void)
{
    static const struct { const char *name; u16 base; int n; } groups[] = {
        { "pal", ROAD_H(ROAD_pal0), 4 }, { "sa1", ROAD_H(ROAD_sa), 4 }, { "pst", ROAD_H(ROAD_pst0), 4 },
        { "rck", ROAD_H(ROAD_rck), 4 }, { "GST", ROAD_H(ROAD_GST0), 5 }, { "sm0", SCN_H(0), 5 },
        { "sm1", SCN_H(5), 5 }, { "md0", SCN_H(10), 5 }, { "lg0", SCN_H(20), 5 }, { "SM", SCN_H(80), 5 },
        { "car1", DS_traffic1_handles, 8 }, { "car1r", DS_traffic1_handles + 64, 8 },
        { "cop", DS_cop_car_handles + 64, 8 }, { "opp", DS_opp_road_handles + 32, 8 },
        { "cp", DS_cop_extra_handles + 64, 32 }, { "rcka", SCN_H(160), 4 }, { "sky", DS_scenery_sky_handles, 14 },
    };
    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; g++) {
        fprintf(stderr, "enh: %-5s", groups[g].name);
        for (int i = 0; i < groups[g].n; i++) {
            const EnhSprite *s = enh_sprite(ds_far((u16)(groups[g].base + 4 * i)));
            if (s) fprintf(stderr, " %dx%d@%d,%d", s->w, s->h, s->hx, s->hy);
            else fprintf(stderr, " -");
        }
        fprintf(stderr, "\n");
    }
    for (int t = 0; t < 16; t++) {
        fprintf(stderr, "enh: scn%-2d", t);
        for (int i = 0; i < 5; i++) {
            const EnhSprite *s = enh_sprite(ds_far((u16)(SCN_H(t * 5 + i))));
            if (s) fprintf(stderr, " %dx%d@%d,%d", s->w, s->h, s->hx, s->hy);
            else fprintf(stderr, " -");
        }
        fprintf(stderr, "\n");
    }
}

static bool same_code(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return false;
    return *a == *b;
}

void enh_debug_stage(void)
{
    const char *e = getenv("TD2_ENH_STAGE");
    if (!e || !*e) return;
    size_t n = strlen(e);
    if (n < 2 || n > 6 || e[n - 1] < '0' || e[n - 1] > '9') return;
    char code[8];
    memcpy(code, e, n - 1);
    code[n - 1] = 0;
    for (s16 i = 0; i < DSS(DS_nscenes); i++) {
        if (!same_code(DSTR(scn_rec(i)), code)) continue;
        s16 st = (s16)(e[n - 1] - '0');
        if (st >= scn_stages(i)) return;
        DSS(DS_scn_idx) = i;
        strcpy(DSTR(DS_scn_code), DSTR(scn_rec(i)));
        DSS(DS_scn_disk) = scn_disk(i);
        DSS(DS_stage) = st;
        DSS(DS_last_stage) = scn_stages(i) == st + 1 ? 1 : 0;
        return;
    }
}

/* TD2_ENH_START=<unit>: the attract mode starts that many road units into the stage, with the region
 * state and the toggles of the units skipped (developer aid; scenery and police state are not replayed). */
static void debug_start(void)
{
    const char *e = getenv("TD2_ENH_START");
    if (!e || DSW(DS_demo_mode) == 0) return;
    int n = atoi(e);
    if (n <= 0 || n >= (int)DSW(DS_dat_road_units) - 300) return;
    u8 sf = DSB(DS_start_flags), la;
    for (int u = 1; u <= n; u++) {
        u8 b = DSB((u16)(ROAD0 + u));
        sf = (u8)(((sf & 0xFE) | (b >> 7)) ^ rec_of(u)[0]);
        u8 code = rec_of(u + 1)[3];
        u16 h = code < 0x30 ? DSW((u16)(DS_OBJECT_JT + 2 * code)) : 0;
        if (h == 0x49C9) DSB(DS_toggle_37f7) ^= 1;
        else if (h == 0x49CF) DSB(DS_median) ^= 1;
        else if (h == 0x49D5) DSB(DS_backdrop_off) ^= 1;
        else if (h == 0x49DB) DSB(DS_scenery_density) = (u8)(DSB(DS_scenery_density) + 0x10);
        else if (h == 0x49E1) DSB(DS_scenery_density) = (u8)(DSB(DS_scenery_density) - 0x10);
    }
    la = 0;
    for (int u = 0; u <= n + ENH_SCENERY_AHEAD; u++) la ^= rec_of(u)[0];
    DSB(DS_lookahead_flags) = la;
    DSB(DS_start_flags) = sf;
    DSW(DS_player_pos) = (u16)(DSW(DS_player_pos) + n);
    if (DSW(DS_opp_pos) != 0) DSW(DS_opp_pos) = (u16)(DSW(DS_opp_pos) + n);
    DSW(DS_distance_left) = (u16)(DSW(DS_distance_left) - n);
    DSW(DS_fuel) = (u16)(DSW(DS_fuel) - n);
}

/* ------------------------------------------------------------------------------------------------ */
/* hooks                                                                                            */

void enh_init(bool on, int rows)
{
    enabled = on;
    if (rows < ENH_MIN_ROWS) rows = ENH_MIN_ROWS;
    if (rows > ENH_MAX_ROWS) rows = ENH_MAX_ROWS;
    enh_rows_setting = rows;
    debug_on = getenv("TD2_ENH_DEBUG") != NULL;
    stats_on = getenv("TD2_ENH_STATS") != NULL;
    compare_dir = getenv("TD2_ENH_COMPARE_DIR");
    const char *tp = getenv("TD2_ENH_TRACE");
    if (tp) trace = fopen(tp, "w");
    if (enabled) gfx_set_overlay(ov_dirty, ov_draw);
}

void enh_stage_begin(void)
{
    if (!enabled) return;
    enh_sprite_cache_clear();
    active = false;
    snap_reset();
    dirty = true;
    if (debug_on) debug_sizes();
    debug_start();
}

void enh_stage_end(void)
{
    if (!enabled) return;
    active = false;
    enh_sprite_cache_clear();
    snap_reset();
    dirty = true;
}

void enh_life_reset(void)
{
    if (!enabled) return;
    snap_reset();
}

void enh_frame(void)
{
    if (!enabled || !enh_raster_setup()) return;
    uint64_t t0 = host_time_ns();
    if (DSB(DS_fall_mode) != 0) {
        /* the falling-off-the-road view stays the original's */
        window_on = false;
        active = true;
        dirty = true;
        return;
    }
    compute_view();
    enh_scene_build(&view, cars, ncars);
    if (trace) {
        double near_car = 0;
        for (int i = 0; i < ncars; i++)
            if (near_car == 0 || cars[i].s < near_car) near_car = cars[i].s;
        fprintf(trace, "%.6f %.4f %.3f %.3f %.3f %.3f %.3f %u %.2f\n", host_time_ns() / 1e9, view.s, view.lat, view.yaw,
                view.heading, near_car, enh_sc.rows[20].y, (unsigned)last.pos, (host_time_ns() - t0) / 1e6);
    }
    enh_raster_render();
    update_cover();
    if (compare_dir) compare_dump();
    window_on = true;
    active = true;
    dirty = true;
    if (stats_on) {
        uint64_t dt = host_time_ns() - t0;
        stat_ns += dt;
        if (dt > stat_max_ns) stat_max_ns = dt;
        stat_cmds += enh_sc.ncmds;
        if (++stat_frames == 300) {
            fprintf(stderr, "enh: %d frames, render %.2f ms avg, %.2f ms max, overlay %.2f ms avg, %d commands avg (%dx%d samples)\n",
                    stat_frames, stat_ns / 1e6 / stat_frames, stat_max_ns / 1e6, ov_ns / 1e6 / stat_frames, stat_cmds / stat_frames,
                    enh_sw, enh_sh);
            stat_frames = stat_cmds = 0;
            stat_ns = stat_max_ns = ov_ns = 0;
        }
    }
}

/* draw_gear_gate counts its close delay in frames (10 at the original's ~15 fps) */
void enh_gear_gate(void)
{
    static uint64_t next_ns;
    if (enabled) {
        uint64_t now = host_time_ns(), period = 1000000000ull / HOST_ORIGINAL_FPS;
        bool due = now >= next_ns;
        if (due) next_ns = (next_ns == 0 || now > next_ns + 4 * period) ? now + period : next_ns + period;
        u8 vis = DSB(DS_dash_toggle);
        if (!due && vis == DSB(DS_gate_prev) && vis != 1 && DSB(DS_gate_close_delay) != 0)
            DSB(DS_gate_close_delay)++;                   /* this frame does not count */
    }
    draw_gear_gate();
}
