/* Enhanced renderer: hooks, simulation snapshots and extrapolation, frame orchestration, coverage of the
 * EGA overlays and the screen overlay (ENHANCED.md). Not bound by the faithful-engine rules.
 *
 * The faithful stage loop keeps drawing its own frame; enh_frame() renders the front view and the mirror
 * again from the simulation state extrapolated to the current time and lays them over rows 19..110 of the
 * EGA frame, except where the original drew something over them (mirror frame, ticket) or on the screen
 * after presenting them (messages, windscreen cracks, GAME OVER). */
#include "enh_internal.h"
#include "../host.h"
#include "../game/flow.h"
#include "../game/scene.h"
#include "../platform/gfx.h"
#include "../platform/res.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROAD0 0x3B51
#define FALL_RATE 2.0                       /* height units the eye drops per unit of fall_scroll */
#define STEP_NS 100000000.0                 /* one 10 Hz simulation step */

int enh_rows_setting = ENH_DEFAULT_ROWS;
bool enh_show_position = false;             /* --show-position on / off (default); F9 toggles */
bool enh_valley = false;                    /* --valley on / off (test default) */
bool enh_detail_max = true;                 /* --sprite-detail max (test default) / auto */
int enh_scenery_ahead = ENH_SCENERY_AHEAD_MAX;
static bool enabled;                        /* false: --classic */
static bool active;                         /* overlay shows a rendered frame */
static bool dirty;

/* ------------------------------------------------------------------------------------------------ */
/* simulation snapshots                                                                             */

#define MAX_TRAFFIC 50

typedef struct {
    bool valid;
    uint64_t t;
    u16 pos, heading, cloud, counter;
    u8 sub, start_flags;
    s16 lat, yaw, raw_yaw, steer;
    u8 fall_mode;
    u16 fall_v;
    double unit_dx;                 /* per-unit lateral sum at the step (see unit samples) */
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
    s->raw_yaw = DSS(DS_yaw);
    s->steer = DSS(DS_steer_angle);
    s->fall_mode = DSB(DS_fall_mode);
    s->fall_v = DSW(DS_fall_scroll);
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

/* ------------------------------------------------------------------------------------------------ */
/* per-unit view samples                                                                            */
/* motion (06c9:4674) changes yaw, view_yaw and player_lateral once per road unit crossed, and a step  */
/* crosses a varying number of units, so these values are sampled per unit and evaluated at the      */
/* continuous road position. The lateral is split into the part motion adds per unit (the yaw term,   */
/* summed in unit_sum) and the rest (pull-over, demo lane changes, resets), which changes per step.   */

static const u8 *rec_of(int u)
{
    long a = (long)ROAD0 + u;
    u8 b = (a >= ROAD0 && a < 0x52C8) ? DSB((u16)a) : 0;
    return mp(DGROUP, (u16)(DS_road_records + (b & 0x7F) * 4));
}

/* Sum of the road curve motion adds to yaw when the car enters units 1..u (road_curve = curve * 64 of the
 * unit the car was on): the part of yaw that only follows the road. */
#define CURVE_UNITS (0x52C8 - ROAD0)
static s32 curve_prefix[CURVE_UNITS + 1];
static int curve_prefix_n;

static double curve_sum_at(int u)
{
    if (u <= 0) return 0;
    if (u > CURVE_UNITS) u = CURVE_UNITS;
    while (curve_prefix_n < u) {
        curve_prefix[curve_prefix_n + 1] = curve_prefix[curve_prefix_n] + (s8)rec_of(curve_prefix_n)[1] * 64;
        curve_prefix_n++;
    }
    return curve_prefix[u];
}

static double curve_sum_s(double s)
{
    int V = (int)floor(s);
    double a = curve_sum_at(V), b = curve_sum_at(V + 1);
    return a + (b - a) * (s - V);
}

#define UNIT_RING 64
typedef struct { int unit; s16 view, yaw; double dx; } UnitSample;
static UnitSample usamp[UNIT_RING];
static int usamp_n, usamp_head;
static double unit_sum;

/* the lateral step of motion for a yaw: player_x -= (s8)((sin_deg(yaw * 2 >> 8) * 36) >> 8) */
static double yaw_dx(s16 yaw)
{
    u8 a8 = (u8)((u16)(yaw << 1) >> 8);
    s32 p = (s32)sin_deg8(a8) * 36;
    return -(double)(s8)(u8)((u32)p >> 8);
}

/* roadside scenery of each road unit as the ring held it when the car entered the unit (the mirror looks
 * further back than the ring keeps them) */
#define HIST_N (0x52C8 - ROAD0 + 1)
static s8 hist_type[HIST_N], hist_off[HIST_N];
static bool hist_ok[HIST_N];

bool enh_scenery_at(int unit, s8 *type, s8 *offset)
{
    if (unit < 0 || unit >= HIST_N || !hist_ok[unit]) return false;
    *type = hist_type[unit];
    *offset = hist_off[unit];
    return true;
}

bool enh_mirror_scenery(u8 k, s8 *type, s8 *offset)
{
    if (!enabled || ENH_SCENERY_AHEAD <= 0x46) return false;
    /* the faithful mirror's row r reads slot unit_phase_m - r; its rows are units P .. P - 24, and after
     * project_mirror its walk pointer is P - 25 */
    int r = (u8)(DSB(DS_unit_phase_m) - k) & 0x7F;
    if (r < 128 - ENH_SCENERY_AHEAD) return false;        /* the ring still holds it */
    int P = (int)(s32)(DSW((u16)(DS_walk_ptr + 0x195C)) + 25 - ROAD0);
    if (enh_scenery_at(P - r, type, offset)) return true;
    *type = -1;
    return true;
}

void enh_unit_step(void)
{
    if (!enabled) return;
    {
        int u = (int)(s32)(DSW(DS_player_pos) - ROAD0);
        u8 k = (u8)(DSB(DS_ring_counter) - 1) & 0x7F;       /* the slot of the unit just entered */
        if (u >= 0 && u < HIST_N) {
            hist_type[u] = DSC((u16)(DS_dat_scenery_type + k));
            hist_off[u] = DSC((u16)(DS_dat_scenery_offset + k));
            hist_ok[u] = true;
        }
    }
    unit_sum += yaw_dx(DSS(DS_yaw));
    UnitSample *u = &usamp[usamp_head];
    u->unit = (int)(s32)(DSW(DS_player_pos) - ROAD0);
    u->view = DSS(DS_view_yaw);
    u->yaw = DSS(DS_yaw);
    u->dx = unit_sum;
    usamp_head = (usamp_head + 1) % UNIT_RING;
    if (usamp_n < UNIT_RING) usamp_n++;
}

static const UnitSample *sample_at(int unit)
{
    for (int k = 1; k <= usamp_n; k++) {
        const UnitSample *u = &usamp[(usamp_head - k + UNIT_RING) % UNIT_RING];
        if (u->unit == unit) return u;
    }
    return NULL;
}

static double fall_dprev;                   /* fall_scroll change of the step before the last */
static void dev_events(void);

void enh_sim_step(void)
{
    dev_events();                           /* also with --classic, for comparisons */
    if (!enabled) return;
    uint64_t t = host_tick_ns();
    if (last.valid) {
        fall_dprev = (double)last.fall_v - prev.fall_v;
        prev = last;
    }
    take(&last, t);
    last.unit_dx = unit_sum;
    if (!prev.valid) prev = last;
}

static int hist_n;                          /* rendered positions, see view_samples */
static double trace_se, trace_rest;         /* trace: lagged read position, steering part of yaw */

static void filt_reset(void);

static void snap_reset(void)
{
    filt_reset();
    prev.valid = last.valid = false;
    usamp_n = 0;
    hist_n = 0;
}

/* view_yaw and the per-unit lateral sum for units lo..lo + TAB_N - 1: recorded up to the unit of the last
 * step; beyond it (only needed when a stall leaves the lagged position ahead of the samples) predicted
 * with motion's formulas and the step's steering */
#define TAB_N 24
typedef struct { int lo; double view[TAB_N], dx[TAB_N], rest[TAB_N]; } UnitTab;
enum { TAB_VIEW, TAB_DX, TAB_REST };

static bool pulled_over_state(void)
{
    u8 cs = DSB(DS_cop_state);
    return cs != 8 && cs >= 2;
}

static void build_tab(UnitTab *t, int P)
{
    t->lo = P - 8;
    const UnitSample *sp = sample_at(P);
    s16 view = sp ? sp->view : last.yaw, yaw = sp ? sp->yaw : last.raw_yaw;
    double dx = sp ? sp->dx : last.unit_dx;
    double hv = view, hd = dx, hr = yaw - curve_sum_at(P);  /* missing units hold the next known value */
    for (int u = P; u >= t->lo; u--) {
        const UnitSample *s = sample_at(u);
        if (s) { hv = s->view; hd = s->dx; hr = s->yaw - curve_sum_at(u); }
        t->view[u - t->lo] = hv;
        t->dx[u - t->lo] = hd;
        t->rest[u - t->lo] = hr;
    }
    s16 st = last.steer, c = DSS(DS_road_curve);
    u8 mph = DSB((u16)(DS_speed + 1));
    u16 v2 = (u16)(mph * mph), lo_w = DSW(DS_car_grip), hi_w = DSW((u16)(DS_car_grip + 2)), lim = lo_w;
    bool fits = (u16)(hi_w << 1) < v2;
    if (fits) lim = div32_16((u32)hi_w << 16 | lo_w, v2, NULL);
    bool demo = DSW(DS_demo_mode) == 1 && !enh_dev_driver();
    s16 ymin = DSS(DS_YAW_MIN), ymax = DSS(DS_YAW_MAX);
    if (pulled_over_state()) st = 0;
    for (int u = P + 1; u < t->lo + TAB_N; u++) {
        if (demo) {
            yaw = 0;
            st = (s16)-(u16)c;
        } else {
            s16 add = st;
            u16 l = lim;
            bool skid = false;
            if (fits) {
                if (st >= 0) skid = st > (s16)l;
                else { l = (u16)-l; skid = st < (s16)l; }
            }
            if (skid) {
                u16 b = (u16)(-(u16)st + l);
                b = (u16)(b + (u16)(l << 1));
                add = (s16)b >> 1;
            }
            yaw = (s16)(yaw + c + add);
            view = yaw;
        }
        if (view > ymax) view = ymax; else if (view < ymin) view = ymin;
        if (yaw > ymax) yaw = ymax; else if (yaw < ymin) yaw = ymin;
        view = (s16)(view >> 2);
        if (pulled_over_state()) { view = 0; yaw = 0; }
        dx += yaw_dx(yaw);
        c = (s16)((s16)((u16)rec_of(u)[1] << 8) >> 2);    /* the curve of the unit just entered */
        t->view[u - t->lo] = view;
        t->dx[u - t->lo] = dx;
        t->rest[u - t->lo] = yaw - curve_sum_at(u);
    }
}

static double tab_eval(const UnitTab *t, double s, int which)
{
    int V = (int)floor(s);
    double f = s - V;
    int i = V - t->lo, j = i + 1;
    i = i < 0 ? 0 : i >= TAB_N ? TAB_N - 1 : i;
    j = j < 0 ? 0 : j >= TAB_N ? TAB_N - 1 : j;
    const double *a = which == TAB_DX ? t->dx : which == TAB_REST ? t->rest : t->view;
    return a[i] + (a[j] - a[i]) * f;
}

/* ------------------------------------------------------------------------------------------------ */
/* view state for this frame                                                                        */

static double pos_of(u16 pos, u8 sub) { return (double)(s32)(pos - ROAD0) + sub / 256.0; }

/* How far behind the car's drawn position the steering part of the view yaw and the lateral are read
 * (see ENHANCED.md "Smooth motion"): that many milliseconds and road units, so that both samples the
 * value is interpolated from are recorded ones. The developer aids TD2_ENH_LAG_MS / _UNITS change them. */
static double lag_ms = 0, lag_units = 0.5, lag_tau = 110;



static EnhView view;
static double mirror_fall;                  /* scroll of the mirror's image in the water */
static const char *compare_dir;             /* developer aid, see enh_init */

/* first-order smoothing of the steering part of the yaw and of the lateral (not of the road curve, which
 * follows the road exactly); a jump resets it */
static double filt_rest, filt_lat;
static uint64_t filt_t;

static void filt_reset(void) { filt_t = 0; }

static void filt_apply(uint64_t now, double *rest, double *lat)
{
    if (compare_dir || lag_tau <= 0) return;              /* compare mode: the original's values */
    double dt = filt_t && now > filt_t ? (double)(now - filt_t) / 1e6 : 0;
    double k = dt > 0 ? 1 - exp(-dt / lag_tau) : 1;
    if (!filt_t || fabs(*rest - filt_rest) > 4000 || fabs(*lat - filt_lat) > 400) k = 1;
    filt_rest += (*rest - filt_rest) * k;
    filt_lat += (*lat - filt_lat) * k;
    filt_t = now;
    *rest = filt_rest;
    *lat = filt_lat;
}

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

/* other cars' laterals: interpolated between the last two steps (they move per step and stop at limits,
 * which extrapolation would overshoot) */
static double lat_interp(s16 p, s16 l, double alpha, double limit)
{
    if (compare_dir || fabs((double)l - p) > limit) return l;
    return p + ((double)l - p) * alpha;
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
        if (r > len / 2) r -= len;                        /* behind the player */
    }
    *d = r;
    return true;
}

static int car_id;

static void add_car(double s, double lat, int kind, u16 type)
{
    if (ncars == ENH_MAX_CARS) return;
    cars[ncars] = (EnhCar){ s, lat, kind, type, ncars, car_id };
    ncars++;
}

static void compute_view(void)
{
    uint64_t now = host_time_ns();
    if (!last.valid) {
        take(&last, now);
        prev = last;
    }
    bool frozen = DSB(DS_run_state) != 0;
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
    {
        /* view_yaw and lateral. motion changes them per road unit (samples), and the lateral also per step
         * (pull-over, demo lane changes: `base`, interpolated between the last two steps). The samples are
         * read at the position the car was drawn at one step ago, one unit back: those units are always
         * recorded, so nothing is predicted and nothing needs correcting.
         * yaw is the road curve summed over the units entered (known for every unit) plus the rest
         * (steering, skidding, clamps). Only the rest is read at the lagged position; the curve part is
         * taken at the car's unit for each of the two blended views, as the original pairs them, so the
         * view does not swing when a bend starts or tightens. view_yaw = yaw >> 2 (the demo keeps its
         * own view_yaw). */
        static UnitTab tab;
        static uint64_t tab_t;
        static double hs[16];
        static uint64_t ht[16];
        int P = (int)(s32)(last.pos - ROAD0);
        if (hist_n == 0 || tab_t != last.t) {
            build_tab(&tab, P);
            tab_t = last.t;
        }
        double base_last = last.lat - last.unit_dx, base_prev = prev.lat - prev.unit_dx;
        double base = compare_dir || fabs(base_last - base_prev) > 300 ? base_last
                                                                        : base_prev + (base_last - base_prev) * alpha;
        double se;
        if (compare_dir) {
            se = floor(s);                                /* the original's values at the car's unit */
        } else {
            if (hist_n && (s < hs[(hist_n - 1) % 16] || s > hs[(hist_n - 1) % 16] + 8)) hist_n = 0;
            double lag = s - ds * (lag_ms / 100.0);
            uint64_t tq = now - (uint64_t)(lag_ms * 1e6);
            for (int k = hist_n - 1; k >= 1 && k >= hist_n - 15; k--) {
                uint64_t ta = ht[(k - 1) % 16], tb = ht[k % 16];
                if (ta <= tq && tq <= tb && tb > ta) {
                    lag = hs[(k - 1) % 16] + (hs[k % 16] - hs[(k - 1) % 16]) * (double)(tq - ta) / (double)(tb - ta);
                    break;
                }
            }
            ht[hist_n % 16] = now;
            hs[hist_n % 16] = s;
            hist_n++;
            se = lag - lag_units;
        }
        int V = (int)floor(s);
        double f = s - V;
        bool demo = DSW(DS_demo_mode) == 1 && !enh_dev_driver();
        double rest = tab_eval(&tab, se, demo ? TAB_VIEW : TAB_REST);
        double lat = base + tab_eval(&tab, se, TAB_DX);
        filt_apply(now, &rest, &lat);
        if (demo) {
            view.yaw_a = view.yaw_b = rest;               /* the demo keeps its own view_yaw */
        } else {
            double ymin = DSS(DS_YAW_MIN), ymax = DSS(DS_YAW_MAX);
            /* The road integrators (enh_scene.c integrate) add the curve from unit org + 1 on, so the two
             * views agree only if their yaws differ by the curve of unit V + 1: the curve sum one unit on.
             * With the original's pairing (yaw of the sum up to V, as its simulation has it) the road beyond
             * the car turned by the change of curve across the last unit before every change - a twitch
             * entering bends, twice as large through S-bends. Compare mode keeps the original's. */
            int co = compare_dir ? 0 : 1;
            double ya = rest + curve_sum_at(V + co), yb = rest + curve_sum_at(V + 1 + co);
            ya = ya < ymin ? ymin : ya > ymax ? ymax : ya;
            yb = yb < ymin ? ymin : yb > ymax ? ymax : yb;
            view.yaw_a = compare_dir ? floor(ya / 4) : ya / 4;
            view.yaw_b = compare_dir ? floor(yb / 4) : yb / 4;
        }
        trace_se = se;
        trace_rest = rest;
        view.yaw = view.yaw_a + (view.yaw_b - view.yaw_a) * f;
        view.lat = view.lat_a = view.lat_b = lat;
    }
    /* falling: fall_scroll grows by a steadily increasing amount per step (3 in the water) */
    view.fall_mode = last.fall_mode;
    view.fall_v = last.fall_v;
    if (last.fall_mode != 0 && prev.fall_mode == last.fall_mode) {
        double dv = (double)last.fall_v - prev.fall_v, dn = dv + (dv - fall_dprev);
        if (dv <= 0 || dn < 0) dn = dv > 0 ? dv : 0;
        view.fall_v = last.fall_v + alpha * dn;
    }
    /* Falling off the road (not in the water): the camera drops below the road, FALL_RATE height units per
     * unit of the original's fall_scroll (about one view pixel at the nearest rows), so the fall keeps the
     * original's timing and end while the scene is the driving view seen from below the road. */
    view.fall_drop = (view.fall_mode != 0 && view.fall_mode != 4) ? view.fall_v * FALL_RATE : 0.0;

    /* the mirror's water image moves up by fall_scroll / 8 at every frame the original draws (~15 Hz) */
    {
        static uint64_t mf_t;
        if (view.fall_mode != 4 || view.fall_v <= 0) {
            mirror_fall = 0;
        } else if (now > mf_t) {
            double dt = (double)(now - mf_t) / 1e9;
            if (dt > 0.1) dt = 0.1;
            mirror_fall += floor(view.fall_v / 8) * HOST_ORIGINAL_FPS * dt;
            if (mirror_fall > MIRROR_H) mirror_fall = MIRROR_H;   /* all colour 9 */
        }
        mf_t = now;
    }
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
    double nr = enh_rows_setting + 2, mr = -(ENH_MIRROR_ROWS(enh_rows_setting) + 2);   /* front / mirror range */
    double d;
    for (int l = 1; l >= 0; l--) {                        /* the original's list order, reversed */
        bool same = last.n[l] == prev.n[l];
        for (int i = last.n[l] - 1; i >= 0; i--) {
            bool hp = same && prev.type[l][i] == last.type[l][i];
            car_rel(last.cpos[l][i], last.csub[l][i], prev.cpos[l][i], prev.csub[l][i], hp, alpha, s, len, &d, 0, 0, 0);
            if (d < mr || d > nr) continue;
            double x = hp ? lat_interp(prev.cx[l][i], last.cx[l][i], alpha, 100) : last.cx[l][i];
            car_id = l * 50 + i;
            add_car(s + d, x, ENH_CAR_TRAFFIC, last.type[l][i]);
        }
    }
    if (DSB(DS_opponent_enabled) != 0) {
        car_rel(last.opp_pos, last.opp_sub, prev.opp_pos, prev.opp_sub, prev.opp_pos != 0, alpha, s, len, &d, 1,
                last.opp_speed, prev.opp_speed);
        car_id = 100;
        if (d >= mr && d <= nr)
            add_car(s + d, lat_interp(prev.opp_lat, last.opp_lat, alpha, 200), ENH_CAR_OPP, 0);
    }
    bool cop_moving = DSB(DS_cop_active) != 0, cop_parked = DSB(DS_cop_state) >= 7;
    if (cop_moving || cop_parked) {
        bool hp = prev.cop_pos != 0 && last.cop_pos != 0;
        car_rel(last.cop_pos, last.cop_sub, prev.cop_pos, prev.cop_sub, hp, alpha, s, len, &d, 1, last.cop_speed,
                prev.cop_speed);
        double x = hp ? lat_interp(prev.cop_lat, last.cop_lat, alpha, 200) : last.cop_lat;
        car_id = 101;
        if (cop_moving && d >= mr && d <= nr) add_car(s + d, x, ENH_CAR_COP, 0);
        if (cop_parked && d >= mr && d <= nr) add_car(s + d, 0, ENH_CAR_PARKED, 0);   /* drawn a row nearer */
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* coverage: pixels of the road window that keep the EGA image                                      */

static u8 cover[VIEW_W * VIEW_H];
static u8 main_snap[4][40 * VIEW_H];        /* main buffer after draw_front */
static u8 main_snap_m[4][40 * VIEW_H];      /* main buffer after draw_mirror */
static u8 vram_snap[4][40 * VIEW_H];
static int mirror_x, mirror_y;              /* mirror position in the window, mirror_x < 0: none */

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

void enh_after_mirror(void)
{
    if (!enabled) return;
    const u8 *pl[4];
    u16 rows[VIEW_H];
    if (!main_planes(pl, rows)) return;
    for (int k = 0; k < 4; k++)
        for (int y = 0; y < VIEW_H; y++) memcpy(main_snap_m[k] + 40 * y, pl[k] + rows[y], 40);
}

static bool in_mirror(int x, int y)
{
    return mirror_x >= 0 && x >= mirror_x && x < mirror_x + MIRROR_W && y >= mirror_y && y < mirror_y + MIRROR_H;
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
    FarPtr ms = ds_far(DS_mirror_sprite);
    mirror_x = -1;
    if (ms.seg != 0) {
        mirror_x = (s16)rd16(ms.seg, (u16)(ms.off + 8));
        mirror_y = (s16)rd16(ms.seg, (u16)(ms.off + 10));
    }
    /* what was drawn into the main buffer after the view: after draw_front, or inside the mirror after
     * draw_mirror */
    if (main_planes(pl, rows)) {
        for (int y = 0; y < VIEW_H; y++)
            for (int bx = 0; bx < 40; bx++) {
                u8 cf = 0, cm = 0;
                for (int k = 0; k < 4; k++) {
                    u8 p = pl[k][(u16)(rows[y] + bx)];
                    cf |= (u8)(p ^ main_snap[k][40 * y + bx]);
                    cm |= (u8)(p ^ main_snap_m[k][40 * y + bx]);
                }
                if (!(cf | cm)) continue;
                for (int b = 0; b < 8; b++) {
                    int x = bx * 8 + b;
                    if ((in_mirror(x, y) ? cm : cf) & (0x80 >> b)) cover[y * VIEW_W + x] = 1;
                }
            }
    }
    /* the mirror frame and the ticket, whatever colours they happen to have */
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
    gfx_screen_written_clear();                           /* the road was just presented */
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

/* ------------------------------------------------------------------------------------------------ */
/* position indicator (--show-position, F9): the stage code and the road unit as TD2_ENH_STAGE and      */
/* TD2_ENH_START take them, and the lateral position, in the top left corner of the road view          */

static const struct { char c; u8 rows[7]; } GLYPHS[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } }, { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } }, { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F } }, { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '=', { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 } },
};

static const u8 *glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++)
        if (GLYPHS[i].c == c) return GLYPHS[i].rows;
    return NULL;                                           /* space and anything else */
}

static void show_position_toggle(void)
{
    enh_show_position = !enh_show_position;
    dirty = true;
}

/* the text: "<stage code><stage> <unit> X<lateral>", e.g. "CCC0 790 X160" */
static void position_text(char *buf, size_t n)
{
    char code[8];
    snprintf(code, sizeof code, "%s", DSTR(DS_scn_code));
    int unit = (int)(s32)(DSW(DS_player_pos) - ROAD0);
    snprintf(buf, n, "%s%d %d X%d", code, DSS(DS_stage), unit, DSS(DS_player_lateral));
}

static void draw_position(u32 *px, int k)
{
    char txt[40];
    position_text(txt, sizeof txt);
    enh_draw_text(px, k, VIEW_Y0 + 2, txt);
}

void enh_draw_text(u32 *px, int k, int row, const char *txt)
{
    int p = k >= 2 ? k / 2 : 1;                            /* output pixels per glyph pixel */
    int len = (int)strlen(txt), cw = 6 * p, ow = VIEW_W * k;
    int x0 = 2 * k, y0 = row * k, bw = len * cw + 3 * p, bh = 11 * p;
    for (int y = y0; y < y0 + bh; y++)                     /* dark backing */
        for (int x = x0; x < x0 + bw && x < ow; x++) {
            u32 c = px[(size_t)y * ow + x];
            px[(size_t)y * ow + x] = ((c >> 2) & 0x3F3F3F);
        }
    for (int i = 0; i < len; i++) {
        const u8 *g = glyph(txt[i]);
        if (!g) continue;
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!(g[gy] & (0x10 >> gx))) continue;
                for (int j = 0; j < p; j++)
                    for (int m = 0; m < p; m++) {
                        int x = x0 + 2 * p + i * cw + gx * p + m, y = y0 + 2 * p + gy * p + j;
                        if (x < ow) px[(size_t)y * ow + x] = 0xFFE680;
                    }
            }
    }
}

static void ov_draw(u32 *px, int k)
{
    if (!active || k != enh_scale || !enh_front.out) return;
    uint64_t t0 = host_time_ns();
    u32 key = enh_palette_key();
    if (key != enh_front.pal_key) enh_resolve(&enh_front);
    if (key != enh_mirror.pal_key) enh_resolve(&enh_mirror);
    int ow = VIEW_W * k;
    for (int y = 0; y < VIEW_H; y++) {
        const u8 *p[4], *s[4];
        for (int n = 0; n < 4; n++) {
            p[n] = gfx_vram_plane(n) + (VIEW_Y0 + y) * 40;
            s[n] = vram_snap[n] + y * 40;
        }
        const u8 *cv = cover + y * VIEW_W;
        const u8 *wr = gfx_screen_written() + (VIEW_Y0 + y) * 40;
        for (int bx = 0; bx < 40; bx++) {
            /* drawn on the screen after the road was presented (message boxes, text, cracks, smoke,
             * GAME OVER, prompts), whether or not the value changed; and any other difference */
            u8 changed = (u8)((p[0][bx] ^ s[0][bx]) | (p[1][bx] ^ s[1][bx]) | (p[2][bx] ^ s[2][bx])
                              | (p[3][bx] ^ s[3][bx]) | wr[bx]);
            for (int b = 0; b < 8; b++) {
                int x = bx * 8 + b;
                if (cv[x] || (changed & (0x80 >> b))) continue;
                const EnhTarget *T = &enh_front;
                int sx = x, sy = y;
                if (in_mirror(x, y)) {
                    T = &enh_mirror;
                    sx = x - mirror_x;
                    sy = y - mirror_y;
                }
                for (int j = 0; j < k; j++) {
                    u32 *d = px + (size_t)((VIEW_Y0 + y) * k + j) * ow + (size_t)x * k;
                    const u32 *src = T->out + (size_t)(sy * k + j) * T->ow + (size_t)sx * k;
                    for (int i = 0; i < k; i++) d[i] = src[i];
                }
            }
        }
    }
    if (enh_show_position) draw_position(px, k);
    ov_ns += host_time_ns() - t0;
}

/* ------------------------------------------------------------------------------------------------ */
/* developer aids                                                                                   */

static bool debug_on, stats_on;
static FILE *trace;                         /* TD2_ENH_TRACE=file: per-frame view values */
/* TD2_ENH_COMPARE_DIR: every 2 s, cmpNNNN.bmp with the enhanced road window and mirror (no extrapolation,
 * whole road units) above the original's window at the same moment, scaled up. */
static void dump_scene(FILE *f, const EnhScene *S)
{
    /* the original's rows of the same view (drawn this frame) */
    int on = S->front ? 60 : 25;
    u16 a_l = S->front ? DS_row_l : DS_row_l_m, a_r = S->front ? DS_row_r : DS_row_r_m;
    u16 a_cx = S->front ? DS_row_cx : DS_row_cx_m, a_sy = S->front ? DS_row_sy : DS_row_sy_m;
    u16 a_clip = S->front ? DS_row_clip : DS_row_clip_m;
    fprintf(f, "orig heading %u view_yaw %d cloud %u / enh %.2f %.3f %.2f\n", DSW(DS_heading), DSS(DS_view_yaw),
            DSW(DS_cloud_scroll), view.heading, view.yaw, view.cloud);
    fprintf(f, "backdrop_off %d median %d style %d\n", DSB(DS_backdrop_off), DSB(DS_median), DSB(DS_toggle_37f7));
    fprintf(f, "cars list %u opp %d/%u cop %d/%u/%u\n", S->front ? DSW(DS_front_draw_list_len) : DSW(DS_mirror_draw_list_len),
            DSB(DS_opponent_enabled), DSW(S->front ? DS_opp_row2 : 0x28C0), DSB(DS_cop_active),
            DSW(S->front ? DS_cop_row2 : 0x28C4), DSB(DS_cop_state));
    fprintf(f, "orig top %d walk %04X\n",DSS((u16)(DS_top_sy + (S->front ? 0 : 0x195C))),
            DSW((u16)(DS_walk_ptr + (S->front ? 0 : 0x195C))));
    for (int i = 0; i < on; i++)
        fprintf(f, "orow %2d y %4d cx %5d L %5d R %5d clip %4d\n", i, DSS((u16)(a_sy + 2 * i)), DSS((u16)(a_cx + 2 * i)),
                DSS((u16)(a_l + 2 * i)), DSS((u16)(a_r + 2 * i)), DSS((u16)(a_clip + 2 * i)));
    fprintf(f, "s %.3f top %.2f/%d any %02X state %02X sky %02X start %02X style %d near %d out_found %d fall %d %.2f\n",
            view.s, S->top_sy, S->top_row, S->r0_any, S->r0_state, S->sky_state, S->start_flags, S->style,
            S->tunnel_nearest, S->tunnel_out_found, view.fall_mode, view.fall_v);
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
    static const char *names[] = { "FILL", "SPRITE", "LINE", "GROUND", "WALLS", "BAND", "MARK", "FACE", "DROP", "FENCE" };
    for (int k = 0; k < S->ncmds; k++) {
        const EnhCmd *c = &S->cmds[k];
        if (c->type == CMD_MARK) continue;
        fprintf(f, "%s c%d op%d clip y %.1f..%.1f x %.1f..%.1f  %.1f %.1f %.1f %.1f a %.2f row %d\n", names[c->type], c->colour, c->op,
                c->cy0, c->cy1, c->cx0, c->cx1, c->x0, c->y0, c->x1, c->y1, c->alpha, c->a);
        if (c->type == CMD_SPRITE && c->spr)
            fprintf(f, "  sprite %04X:%04X %dx%d drawn %.1fx%.1f car z %.2f\n", c->spr->seg, c->spr->off, c->spr->w,
                    c->spr->h, c->spr->w * c->x1, c->spr->h * c->x1, c->a < 0 ? (-1 - c->a) / 100.0 : 0.0);
    }
}

static void compare_dump(void)
{
    static uint64_t last_ns;
    static int n;
    uint64_t now = host_time_ns();
    static uint64_t every = 2000000000ull;
    if (n == 0 && getenv("TD2_ENH_COMPARE_MS")) every = (uint64_t)atoi(getenv("TD2_ENH_COMPARE_MS")) * 1000000ull;
    if (n && now - last_ns < every) return;
    last_ns = now;
    const EnhTarget *F = &enh_front, *M = &enh_mirror;
    int k = enh_scale, w = F->ow, h = 2 * F->oh;
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
            if (y < F->oh) {
                int mx = x - mirror_x * k, my = y - mirror_y * k;
                if (mirror_x >= 0 && mx >= 0 && mx < M->ow && my >= 0 && my < M->oh)
                    c = M->out[(size_t)my * M->ow + mx];
                else
                    c = F->out[(size_t)y * F->ow + x];
            } else {
                int oy = (y - F->oh) / k + VIEW_Y0, ox = x / k;
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
    dump_scene(f, &enh_sc);
    fclose(f);
    snprintf(path, sizeof path, "%s/cmp%04d_m.txt", compare_dir, n - 1);
    f = fopen(path, "w");
    if (!f) return;
    dump_scene(f, &enh_mc);
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
        { "oppm", DS_opp_front_handles + 32, 8 }, { "copm", DS_cop_car_handles, 8 },
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

/* the stage's colours and the unit ranges of its tunnels, cliffs and drop-offs (region state walked from the
 * start of the road) */
static void debug_stage_map(void)
{
    static const struct { const char *name; u8 mask; } kinds[] = {
        { "tunnel", 0x80 }, { "cliff L", 0x40 }, { "drop L", 0x20 }, { "cliff R", 0x08 }, { "drop R", 0x04 },
    };
    int n = DSW(DS_dat_road_units);
    fprintf(stderr, "enh: stage %d units, colours left %u right %u shoulder %u sky %u far %u\n", n,
            DSW(DS_scene_words) & 15, DSW(DS_col_right) & 15, DSW(DS_col_shoulder) & 15, DSW(DS_col_sky) & 15,
            DSW(DS_col_far) & 15);
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        fprintf(stderr, "enh: %-8s", kinds[k].name);
        u8 st = 0;
        int from = -1;
        for (int u = 0; u <= n; u++) {
            if (u < n) st ^= rec_of(u)[0];
            bool on = u < n && (st & kinds[k].mask);
            if (on && from < 0) from = u;
            if (!on && from >= 0) { fprintf(stderr, " %d-%d", from, u - 1); from = -1; }
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

bool enh_select_stage(const char *e)
{
    if (!e || !*e) return false;
    size_t n = strlen(e);
    if (n < 2 || n > 6 || e[n - 1] < '0' || e[n - 1] > '9') return false;
    char code[8];
    memcpy(code, e, n - 1);
    code[n - 1] = 0;
    for (s16 i = 0; i < DSS(DS_nscenes); i++) {
        if (!same_code(DSTR(scn_rec(i)), code)) continue;
        s16 st = (s16)(e[n - 1] - '0');
        if (st >= scn_stages(i)) return false;
        DSS(DS_scn_idx) = i;
        strcpy(DSTR(DS_scn_code), DSTR(scn_rec(i)));
        DSS(DS_scn_disk) = scn_disk(i);
        DSS(DS_stage) = st;
        DSS(DS_last_stage) = scn_stages(i) == st + 1 ? 1 : 0;
        return true;
    }
    return false;
}

void enh_debug_stage(void)
{
    const char *lv = getenv("TD2_ENH_LIVES");
    if (lv && atoi(lv) > 0) DSS(DS_lives) = (s16)atoi(lv);
    enh_select_stage(getenv("TD2_ENH_STAGE"));
}

/* TD2_ENH_DRIVER: a steering controller for the attract mode (see enhanced.h) */
static int dev_driver_mode;                 /* 0 off, 1 follow, 2 weave, 3 lazy, 4-6 off the road */

bool enh_dev_driver(void) { return dev_driver_mode != 0 && DSW(DS_demo_mode) == 1; }

void enh_dev_steer(void)
{
    if (!enh_dev_driver()) return;
    double x = DSS(DS_player_lateral), yaw = DSS(DS_yaw), c = DSS(DS_road_curve), st = DSS(DS_steer_angle);
    double xt = 160;
    if (dev_driver_mode == 2) xt = ((DSW(DS_sim_tick10) / 30) & 1) ? 20 : 300;
    if (dev_driver_mode == 4) xt = -900;                  /* off the road to the left */
    if (dev_driver_mode == 5) xt = 1100;                  /* off the road to the right */
    static bool dev_committed;                            /* water: up to speed, then pushed right */
    if (dev_driver_mode == 6 && (dev_committed || DSW(DS_speed) >= 0x6000)) { dev_committed = true; xt = 4000; DSS(DS_player_lateral) = (s16)(x + 30); }
    double yd = 10 * (x - xt);
    double ylim = dev_driver_mode == 6 ? 6000 : dev_driver_mode >= 4 ? 3000 : 1500;
    if (yd > ylim) yd = ylim;
    if (yd < -ylim) yd = -ylim;
    double sd = 0.3 * (yd - yaw) - c;
    if (sd > 0xE00) sd = 0xE00;
    if (sd < -0xE00) sd = -0xE00;
    s8 in = sd > st + 200 ? 1 : sd < st - 200 ? -1 : 0;
    if (dev_driver_mode == 3 && DSW(DS_sim_tick10) % 10 > 1) in = 0;   /* lazy: steers only now and then */
    DSB(DS_steer_in) = (u8)in;
}

/* TD2_ENH_EVENTS="<step>:<result>,...": sets the drive result (DS:5490) that many simulation steps after
 * the stage start, to reach every result message in the attract mode. TD2_ENH_LIVES=<n>: lives in the
 * attract mode (lives-left messages instead of GAME OVER). */
static int dev_step;

static void dev_events(void)
{
    static const char *spec;
    static bool checked;
    if (!checked) { spec = getenv("TD2_ENH_EVENTS"); checked = true; }
    dev_step++;
    if (!spec || !*spec || DSW(DS_demo_mode) != 1) return;
    char *end;
    long at = strtol(spec, &end, 10);
    if (end == spec || *end != ':') { spec = NULL; return; }
    if (dev_step < at) return;
    long code = strtol(end + 1, &end, 10);
    if (DSB(DS_run_state) == 0) DSB(DS_run_state) = (u8)code;
    spec = *end == ',' ? end + 1 : NULL;
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
    enh_scenery_ahead = enabled ? ENH_SCENERY_AHEAD_MAX : 0x46;
    if (getenv("TD2_ENH_LAG_MS")) lag_ms = atof(getenv("TD2_ENH_LAG_MS"));
    if (getenv("TD2_ENH_LAG_UNITS")) lag_units = atof(getenv("TD2_ENH_LAG_UNITS"));
    if (getenv("TD2_ENH_LAG_TAU")) lag_tau = atof(getenv("TD2_ENH_LAG_TAU"));
    debug_on = getenv("TD2_ENH_DEBUG") != NULL;
    stats_on = getenv("TD2_ENH_STATS") != NULL;
    compare_dir = getenv("TD2_ENH_COMPARE_DIR");
    const char *drv = getenv("TD2_ENH_DRIVER");
    if (drv)
        dev_driver_mode = !strcmp(drv, "weave") ? 2 : !strcmp(drv, "lazy") ? 3 : !strcmp(drv, "offleft") ? 4
                        : !strcmp(drv, "offright") ? 5 : !strcmp(drv, "offwater") ? 6 : 1;
    const char *tp = getenv("TD2_ENH_TRACE");
    if (tp) trace = fopen(tp, "w");
    if (enabled) {
        gfx_set_overlay(ov_dirty, ov_draw);
        host_set_toggle_key(show_position_toggle);
    }
}

void enh_stage_begin(void)
{
    dev_step = 0;
    debug_start();                          /* also with --classic, for comparisons */
    if (!enabled) return;
    enh_sprite_cache_clear();
    active = false;
    snap_reset();
    curve_prefix_n = 0;
    memset(hist_ok, 0, sizeof hist_ok);
    dirty = true;
    if (debug_on) {
        debug_sizes();
        debug_stage_map();
    }
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
    compute_view();
    enh_mirror_build(&view, cars, ncars, mirror_fall);
    enh_scene_build(&view, cars, ncars);
    if (trace) {
        /* time s lat yaw heading | road centre x at 10 / 30 / 60 units ahead | tracked car id, x, depth */
        static int track = -1;
        const EnhCar *tc = NULL;
        for (int i = 0; i < ncars; i++)
            if (cars[i].id == track && cars[i].s - view.s < 60) tc = &cars[i];
        if (!tc) {
            for (int i = 0; i < ncars; i++) {
                double d = cars[i].s - view.s;
                if (d > 8 && d < 50 && (!tc || d < tc->s - view.s)) tc = &cars[i];
            }
            track = tc ? tc->id : -1;
        }
        double cxv = tc ? enh_scene_screen_x(tc->s, tc->lat) : NAN;
        /* ... | camera heading (road curve sum / 4 - view_yaw) drawn, and the simulation's at its last step
         * (what the original draws), steering angle, road curve */
        int P = (int)(s32)(last.pos - ROAD0);
        fprintf(trace, "%.6f %.4f %.3f %.3f %.3f %.3f %.3f %.3f %d %.3f %.2f %u %.2f %.2f %.2f %d %d %.3f %.1f\n",
                host_time_ns() / 1e9, view.s, view.lat, view.yaw, view.heading, enh_scene_screen_x(view.s + 10, 0),
                enh_scene_screen_x(view.s + 30, 0), enh_scene_screen_x(view.s + 60, 0), track, cxv,
                tc ? tc->s - view.s : 0.0, (unsigned)last.pos, (host_time_ns() - t0) / 1e6,
                curve_sum_s(view.s + (compare_dir ? 0 : 1)) / 4 - view.yaw, curve_sum_at(P) / 4 - last.yaw, last.steer, DSS(DS_road_curve),
                trace_se, trace_rest);
    }
    enh_raster_render(&enh_front);
    enh_raster_render(&enh_mirror);
    update_cover();
    if (compare_dir) compare_dump();
    active = true;
    dirty = true;
    if (stats_on) {
        uint64_t dt = host_time_ns() - t0;
        stat_ns += dt;
        if (dt > stat_max_ns) stat_max_ns = dt;
        stat_cmds += enh_sc.ncmds + enh_mc.ncmds;
        if (++stat_frames == 300) {
            fprintf(stderr, "enh: %d frames, render %.2f ms avg, %.2f ms max, overlay %.2f ms avg, %d commands avg (%dx%d samples)\n",
                    stat_frames, stat_ns / 1e6 / stat_frames, stat_max_ns / 1e6, ov_ns / 1e6 / stat_frames, stat_cmds / stat_frames,
                    enh_front.sw, enh_front.sh);
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
