/* Map viewer (--viewer, ENHANCED.md "Map viewer"): a camera flown along a stage, with the enhanced road view
 * filling the whole window - no cockpit, mirror, HUD, other cars or simulation. Not part of the game: only
 * reached with --viewer, which skips the game's menus (game/flow.c game_main, flow_stage.c run_viewer).
 *
 * The stage is loaded as for driving (run_game_load_stage, stage_load), then the timer routines are removed
 * so nothing is simulated. Every frame the front view is built from a render-only view state (EnhView) at
 * the camera's road position and lateral, 320 x 200 instead of 320 x 92 (enh_view_top / enh_view_bottom),
 * and drawn over the whole screen by an overlay of its own. What the simulation keeps as the car drives is
 * derived from the road for any unit instead, so the camera can go backwards and jump:
 *   - the region state (start_flags), the toggles of the road objects (median, tunnel style, backdrop) and
 *     the mountain / cloud scroll, replayed from the stage start as motion does it;
 *   - the camera heading: the road curve part of yaw only (compute_view's split, the steering part is 0),
 *     so the view looks along the road;
 *   - the roadside scenery ring: the placed objects (06c9:499a, as enh_place writes them) and, for the
 *     random scenery, spawn_scenery's rules (density objects, region and right-zone tests, the type choice)
 *     with a hash of the unit instead of rand8, so a unit always shows the same objects. The first 71 units
 *     keep the ring the stage's DAT starts with. The ring slots around the camera are rewritten every frame. */
#include "enh_internal.h"
#include "../host.h"
#include "../game/flow.h"
#include "../game/scene.h"
#include "../platform/gfx.h"
#include "../platform/timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *enh_viewer_stage;
int enh_viewer_start;
int enh_view_top, enh_view_bottom;

#define ROAD0 0x3B51
#define UNITS (0x52C8 - ROAD0)                /* road bytes of the stage image */
/* rows added above and below the view so that it fills the screen: the original's has 51 rows above the
 * horizon and 41 below, the viewer's 121 and 79 (the road reaching down to 2.2 units ahead instead of 4) */
#define VIEW_TOP 70
#define VIEW_BOTTOM (200 - VIEW_H - VIEW_TOP)
#define START_LAT 160.0                       /* player_lateral at a stage start (life_reset: 0xA0) */
#define LAT_MAX 12000.0                       /* how far out to the sides (the road's half-width is ~400) */
#define SPEED 15.0                            /* road units per second (Up / Down; about 130 mph) */
#define SIDE_SPEED 600.0                      /* lateral units per second (Left / Right) */
#define FAST 10.0                             /* with Shift */
#define JUMP 100                              /* road units (PageUp / PageDown) */
#define DAT_RING 0x47                         /* ring slots 0..0x46 (units 1..71) come from the DAT */

static int nunits;                            /* the stage's road units */
static u8 flags_at[UNITS + 2];                /* region state after entering unit u (start_flags) */
static u8 median_at[UNITS + 2], style_at[UNITS + 2], backdrop_at[UNITS + 2];   /* object toggles at unit u */
static double heading_at[UNITS + 2], cloud_at[UNITS + 2];                        /* mountain / cloud scroll */
static s32 curve_at[UNITS + 3];               /* road curve * 64 summed over units 0..u - 1 */
static u8 sc_type[UNITS + 2], sc_side[UNITS + 2];                                /* roadside scenery of unit u */

static double cam_s, cam_lat;                 /* the camera: road position (units) and lateral */

/* ------------------------------------------------------------------------------------------------ */
/* the road                                                                                         */

static u8 road_byte(int u)
{
    long a = (long)ROAD0 + u;
    return (a >= ROAD0 && a < ROAD0 + UNITS) ? DSB((u16)a) : 0;
}

static const u8 *rec(int u) { return mp(DGROUP, (u16)(DS_road_records + (road_byte(u) & 0x7F) * 4)); }

static u16 handler(u8 code) { return code < 0x30 ? DSW((u16)(DS_OBJECT_JT + 2 * code)) : 0; }

static u32 unit_hash(u32 x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* spawn_scenery (sim_motion.c) for unit u, with a hash for rand8: la = the region flags of units 0..u - 1
 * (lookahead_flags when the unit's slot is filled), density = scenery_density with the density objects
 * of units 2..u - 71 applied. Returns the type (0xFF: none) and sets the side. */
static u8 random_scenery(int u, u8 la, u8 density, u8 *side)
{
    u32 h = unit_hash((u32)u * 0x9E3779B1u + (u32)DSS(DS_stage) * 0x85EBCA6Bu);
    if ((u8)h >= density) return 0xFF;
    u8 al = (u8)((h >> 8) & 0x0F);
    if (al == 0x0F || al == 7) return 0xFF;
    if (al > 7) {                                         /* right side */
        if (la & 0x8C) return 0xFF;
        u16 zu = (u16)(u + 28);                           /* the simulation's unit + 150, the car being 122 back */
        for (u16 si = 0; DSW((u16)(DS_right_zones + si)) != 0; si = (u16)(si + 8)) {
            if (zu <= DSW((u16)(DS_right_zones + 2 + si))) {
                if (zu >= DSW((u16)(DS_right_zones + si))) return 0xFF;
                break;
            }
        }
    } else if (la & 0xE0) {
        return 0xFF;
    }
    u8 t = (u8)(((h >> 16) & 0xFF) % 6 * 5);
    if (DSW((u16)(DS_scenery_sprites + ((u16)t << 2))) == 0) return 0xFF;
    *side = (u8)(al - 7);                                 /* -7..-1, 1..7 */
    return t;
}

/* everything the view needs per unit, from the state stage_load left */
static void tables_setup(void)
{
    nunits = DSW(DS_dat_road_units);
    if (nunits < 2 || nunits > UNITS) nunits = UNITS;
    u8 sf = DSB(DS_start_flags), med = DSB(DS_median), sty = DSB(DS_toggle_37f7), bko = DSB(DS_backdrop_off);
    u8 la = 0, density = DSB(DS_scenery_density);
    double hd = 0, cl = 0;
    curve_at[0] = 0;
    for (int u = 0; u <= UNITS + 1; u++) {
        if (u >= 1) {                                     /* motion entering unit u */
            u8 b = road_byte(u);
            sf = (u8)(((sf & 0xFE) | (b >> 7)) ^ rec(u)[0]);
            s16 h = (s16)((s8)rec(u)[1] >> 1);
            hd += h;
            cl += (s16)((h >> 2) + h);
            u16 o = handler(rec(u + 1)[3]);               /* object_dispatch: the next unit's object */
            if (o == 0x49C9) sty ^= 1;
            else if (o == 0x49CF) med ^= 1;
            else if (o == 0x49D5) bko ^= 1;
        }
        flags_at[u] = sf;
        median_at[u] = med;
        style_at[u] = sty;
        backdrop_at[u] = bko;
        heading_at[u] = hd;
        cloud_at[u] = cl;
        curve_at[u + 1] = curve_at[u] + (s8)rec(u)[1] * 64;

        /* scenery: the DAT's ring for the first units, then the stand-in for the random scenery with the
         * placed objects over it */
        if (u >= 73) {
            u16 o = handler(rec(u - 71)[3]);
            if (o == 0x49DB) density = (u8)(density + 0x10);
            else if (o == 0x49E1) density = (u8)(density - 0x10);
        }
        u8 t = 0xFF, side = 0xFF;
        if (u >= 1 && u <= DAT_RING) {
            t = DSB((u16)(DS_dat_scenery_type + u - 1));
            side = DSB((u16)(DS_dat_scenery_offset + u - 1));
        } else if (u > DAT_RING) {
            t = random_scenery(u, la, density, &side);
            if (t == 0xFF) side = 0xFF;
            u8 code = rec(u - 69)[3];                     /* obj_place_roadside, 69 units before */
            if (handler(code) == 0x499A) {
                t = (u8)((code - 0x16) * 5);
                side = DSB((u16)(DS_ROADSIDE_SIDE_BASE + code));
            }
        }
        sc_type[u] = t;
        sc_side[u] = side;
        la ^= rec(u)[0];
    }
}

/* the ring slots of units v - 5 .. v + 122 (the scene reads slot phase = unit - 1, see view_at) */
static void ring_fill(int v)
{
    for (int u = v - 5; u <= v + 122; u++) {
        u16 k = (u16)((u - 1) & 0x7F);
        bool in = u >= 1 && u <= UNITS + 1;
        DSB((u16)(DS_dat_scenery_type + k)) = in ? sc_type[u] : 0xFF;
        DSB((u16)(DS_dat_scenery_offset + k)) = in ? sc_side[u] : 0xFF;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* the view                                                                                         */

static double curve_sum_s(double s)
{
    int V = (int)floor(s);
    if (V < 0) return 0;
    if (V > UNITS + 1) return curve_at[UNITS + 2];
    return curve_at[V] + (curve_at[V + 1] - curve_at[V]) * (s - V);
}

static void view_at(EnhView *v)
{
    memset(v, 0, sizeof *v);
    double s = cam_s;
    int V = (int)floor(s);
    double f = s - V;
    v->s = s;
    /* compute_view with the steering part of yaw 0: the views of units V and V + 1 take the curve sum one
     * unit on (the road integrators add the curve from the unit after the car's on), relative to the
     * curve sum at the camera, so the camera looks along the road and turns with it continuously */
    double c = curve_sum_s(s + 1);
    v->yaw_a = (curve_at[V + 1] - c) / 4;
    v->yaw_b = (curve_at[V + 2] - c) / 4;
    v->yaw = v->yaw_a + (v->yaw_b - v->yaw_a) * f;
    v->lat = v->lat_a = v->lat_b = cam_lat;
    v->heading = heading_at[V] + (heading_at[V + 1] - heading_at[V]) * f;
    v->cloud = cloud_at[V] + (cloud_at[V + 1] - cloud_at[V]) * f;
    /* as after a drive from the stage start: the ring counter is the unit (phase of unit u = u - 1) */
    v->pos = (u16)(ROAD0 + V);
    v->counter = (u16)V;
    v->start_flags = flags_at[V];
    DSB(DS_median) = median_at[V];
    DSB(DS_toggle_37f7) = style_at[V];
    DSB(DS_backdrop_off) = backdrop_at[V];
    ring_fill(V);
}

/* ------------------------------------------------------------------------------------------------ */
/* overlay: the rendered view over the whole screen                                                  */

static bool vdirty;

static bool ov_dirty(void)
{
    bool d = vdirty;
    vdirty = false;
    return d;
}

static void ov_draw(u32 *px, int k)
{
    if (k != enh_scale || !enh_front.out || enh_front.oh != 200 * k) return;
    if (enh_palette_key() != enh_front.pal_key) enh_resolve(&enh_front);
    memcpy(px, enh_front.out, (size_t)enh_front.ow * enh_front.oh * sizeof *px);
    if (enh_show_position) {
        char code[8], txt[48];
        snprintf(code, sizeof code, "%s", DSTR(DS_scn_code));
        snprintf(txt, sizeof txt, "%s%d %d X%d", code, DSS(DS_stage), (int)floor(cam_s), (int)lround(cam_lat));
        enh_draw_text(px, k, 2, txt);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* keys (XT scan codes, also from TD2_KEYS)                                                          */

enum { XT_ESC = 0x01, XT_LSHIFT = 0x2A, XT_RSHIFT = 0x36, XT_HOME = 0x47, XT_UP = 0x48, XT_PGUP = 0x49,
       XT_LEFT = 0x4B, XT_RIGHT = 0x4D, XT_END = 0x4F, XT_DOWN = 0x50, XT_PGDN = 0x51 };

static bool down[0x80];
static bool quit, go_home, go_end;
static int jump;

static void on_scan(u8 xt)
{
    u8 c = xt & 0x7F;
    bool make = !(xt & 0x80);
    down[c] = make;
    if (!make) return;
    switch (c) {                                          /* on every press, key repeats included */
    case XT_ESC: quit = true; break;
    case XT_PGUP: jump += JUMP; break;
    case XT_PGDN: jump -= JUMP; break;
    case XT_HOME: go_home = true; break;
    case XT_END: go_end = true; break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ */

void enh_viewer_run(void)
{
    stage_load();                                         /* colours, sprites, the stage's state */
    timer_install_drive();                                /* no timer routines: no simulation, no sound */
    host_speaker(0, false);
    gfx_set_palette(DS_pal_normal);
    enh_stage_begin();
    tables_setup();
    enh_view_top = VIEW_TOP;
    enh_view_bottom = VIEW_BOTTOM;
    enh_front.vh = VIEW_H + VIEW_TOP + VIEW_BOTTOM;
    gfx_set_overlay(ov_dirty, ov_draw);
    host_set_scan_handler(on_scan);

    double s_max = nunits - 1;
    double start = enh_viewer_start < 0 ? 0 : enh_viewer_start > s_max ? s_max : enh_viewer_start;
    cam_s = start;
    cam_lat = START_LAT;
    bool drawn = false, shown = enh_show_position;
    uint64_t t = host_time_ns();
    while (!quit) {
        host_frame_begin();
        uint64_t now = host_time_ns();
        double dt = (double)(now - t) / 1e9;
        t = now;
        if (dt > 0.1) dt = 0.1;
        double s0 = cam_s, l0 = cam_lat;
        double m = down[XT_LSHIFT] || down[XT_RSHIFT] ? FAST : 1;
        cam_s += ((int)down[XT_UP] - (int)down[XT_DOWN]) * SPEED * m * dt + jump;
        cam_lat += ((int)down[XT_RIGHT] - (int)down[XT_LEFT]) * SIDE_SPEED * m * dt;
        jump = 0;
        if (go_home) { cam_s = start; cam_lat = START_LAT; }
        if (go_end) cam_s = s_max;
        go_home = go_end = false;
        cam_s = cam_s < 0 ? 0 : cam_s > s_max ? s_max : cam_s;
        cam_lat = cam_lat < -LAT_MAX ? -LAT_MAX : cam_lat > LAT_MAX ? LAT_MAX : cam_lat;
        if (drawn && cam_s == s0 && cam_lat == l0 && shown == enh_show_position) continue;
        if (!enh_raster_setup()) break;
        EnhView v;
        EnhCar none[1];
        view_at(&v);
        enh_scene_build(&v, none, 0);
        enh_raster_render(&enh_front);
        drawn = true;
        shown = enh_show_position;
        vdirty = true;
    }
    enh_stage_end();
}
