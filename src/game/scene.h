#pragma once
/* scene_render (06c9:0000-06c9:3fff of TD2EGA.EXE): stage runner, road / scenery projection and
 * drawing (front view and rear-view mirror), cockpit, HUD, crash / smoke / result messages —
 * port/spec/scene_render.md. run_stage() is declared in game.h; this header declares the module's
 * other functions (named as in symbols.h) and its private helpers. All state is in mem[].
 *
 * Files: scene.c (runner, loading, snapshots, top-level views, sequences, messages),
 *        scene_project.c (row projection, spans, cut lines), scene_draw.c (sky, ground, tunnel walls),
 *        scene_objects.c (per-row objects), scene_cockpit.c (cockpit and HUD). */
#include "game.h"
#include "../platform/gfx.h"

/* ---- scene.c */
void main_view_load(void);              /* 06c9:0002 */
void main_view_free(void);              /* 06c9:0083 */
void present_main_view(void);           /* 06c9:0094 */
void project_front(void);               /* 06c9:00aa */
void draw_front(void);                  /* 06c9:00e7 */
void snapshot_front(void);              /* 06c9:0201 */
void stage_load(void);                  /* 06c9:1c8f */
void life_reset(void);                  /* 06c9:1e31 */
void prepare_traffic_lists(void);       /* 06c9:1e59 */
void mirror_load(void);                 /* 06c9:1ee2 */
void mirror_free(void);                 /* 06c9:1f60 */
void project_mirror(void);              /* 06c9:1f71 */
void draw_mirror(void);                 /* 06c9:1f99 */
void snapshot_mirror(void);             /* 06c9:2089 */
void crash_sequence(void);              /* 06c9:3532 */
void message_wait_tail(void);           /* 06c9:362f */
void engine_smoke_sequence(void);       /* 06c9:3643 */
void msg_missed_gas(void);              /* 06c9:36e0 */
void msg_too_far_left(void);            /* 06c9:3713 */
void msg_engine_dead(void);             /* 06c9:371f */
void msg_suspension_dead(void);         /* 06c9:372b */
void msg_steering_dead(void);           /* 06c9:3737 */
void msg_too_much_damage(void);         /* 06c9:3743 */
void msg_fill_er_up(void);              /* 06c9:374f */
void msg_lives_left(void);              /* 06c9:375f */
void message_box_06C9_3788(void);       /* 06c9:3788 */
void select_main_view(void);            /* 06c9:3efc */
void wait_after_message(void);          /* 06c9:3f0d */

/* ---- scene_cockpit.c */
void cockpit_load(void);                /* 06c9:37d0 */
void cockpit_free(void);                /* 06c9:38d4 */
void cockpit_reset(void);               /* 06c9:38f2 */
void draw_hud(void);                    /* 06c9:391f */
void draw_gear_gate(void);              /* 06c9:3b95 */
void draw_steering(void);               /* 06c9:3c3a */
void draw_instruments(void);            /* 06c9:3cef */

/* ---- views (scene_project.c / scene_draw.c / scene_objects.c)
 * The mirror code (06c9:1f71..3531) is a copy of the front code (06c9:00aa..1b2b) with its own row
 * arrays, tables and constants, and its scalars at the front address + 0x195C. The port implements each
 * pair once, parameterised by a SceneView; the differences are marked `front` / `!front` in the code. */
typedef struct SceneView {
    bool front;
    u16 sc;                 /* scalar offset: DS_x + sc (0 front, 0x195C mirror) */
    u16 nrows2;             /* rows * 2: 0x78 / 0x32 */
    s16 width;              /* view width: 320 / 80 */
    s16 height;             /* view height: 92 / 17 */
    s16 horizon;            /* row y offset: 0x33 / 8 */
    s16 centre;             /* row x offset: 0x7D / 0x28 */
    u16 band_default;       /* right band default: 0x0A00 / 0x0280 */
    s16 cut_offset;         /* cliff cut widening: 22 / 6 */
    u16 out_row_default;    /* tunnel_out_row default: 0x76 / 0x30 */
    s16 sky_cut;            /* tunnel portal sky cut offset: 15 / 3 */
    s16 portal_y;           /* tunnel portal sprite y: 0x5B / 0x10 */
    u16 skip_target;        /* DS:0702 value: 0x0B7B / 0x28BD */
    u16 row_ol, row_l, row_cx, row_r, row_or, row_sy, row_clip, row_flags, row_state, row_band;
    u16 xs, ys, w, carscale;            /* DS tables */
    u16 desc, sprite, rowtab;           /* DS: buffer descriptor (far), sprite (far), row table (CS offset) */
    u16 sky_handles;                    /* DS:0704 / DS:27BE */
    u16 draw_list, draw_list_len;
    u16 opp_row2, opp_lat, cop_row2, cop_lat;
    u16 opp_alt;                        /* DS:09AE / DS:2A1C */
} SceneView;

extern const SceneView scene_front_view, scene_mirror_view;

/* Scalars of a view (front symbol + view offset). */
#define VW(v, name) DSW((u16)(DS_##name + (v)->sc))
#define VS(v, name) DSS((u16)(DS_##name + (v)->sc))
#define VB(v, name) DSB((u16)(DS_##name + (v)->sc))
/* Per-row arrays (si = row * 2). */
#define RW(v, arr, si) DSS((u16)((v)->arr + (si)))
#define RU(v, arr, si) DSW((u16)((v)->arr + (si)))
#define RB(v, arr, si) DSB((u16)((v)->arr + (si)))

/* Span arrays (per scanline, shared by both views), index y2 = y * 2. */
#define SPAN(arr, y2) DSS((u16)(DS_##arr + (y2)))

void project_rows(const SceneView *v);          /* 06c9:0338 / 06c9:21c3 */
void build_spans(const SceneView *v);           /* 06c9:05f8 / 06c9:2477 */
void interp_span(s16 a, s16 b, u16 dst_ds, u16 cx, u16 dx);  /* 06c9:06c3 */
void fix_cut_lines(const SceneView *v);         /* 06c9:0746 / 06c9:2542 */
void clamp_spans(u16 ax_first, u16 bp_end);     /* 06c9:088e */
s16  clamp_edge(const SceneView *v, s16 ax, u16 si);   /* 06c9:08cb / 06c9:268a */

void draw_sky(const SceneView *v);              /* 06c9:0919 / 06c9:26d8 */
void draw_ground(const SceneView *v);           /* 06c9:0b0d / 06c9:284f */
void draw_tunnel_walls(const SceneView *v);     /* 06c9:0bb8 / 06c9:28f9 */
void hline(s16 dx, s16 ax, s16 di, u16 bx);     /* 06c9:0d01 */
void tunnel_rib(const SceneView *v, u16 si);    /* 06c9:1ad6 / 06c9:34dc */

void draw_front_objects(void);                  /* 06c9:0d13 */
void draw_mirror_objects(void);                 /* 06c9:2958 */

/* ---- small helpers */
static inline FarPtr hnd(u16 ds_off) { return ds_far(ds_off); }     /* far handle stored at DS:off */
#define CARW(o) DSW((u16)(DS_car + (o)))
#define CARS(o) DSS((u16)(DS_car + (o)))
#define CARB(o) DSB((u16)(DS_car + (o)))

/* Handle tables (§3.5): DS offset of entry k. */
#define ROAD_H(k)   ((u16)(DS_road_handles + 4 * (k)))
#define DASH_H(k)   ((u16)(DS_dash_handles + 4 * (k)))
#define DIG_H(k)    ((u16)(DS_dash_digit_handles + 4 * (k)))
#define SCN_H(k)    ((u16)(DS_scenery_handles + 4 * (k)))

enum {  /* DS:2FAC <car>DASH */
    DASH_dash, DASH_dot, DASH_dota, DASH_gbo0, DASH_gbox, DASH_gnob, DASH_gnab, DASH_inl1, DASH_inl2,
    DASH_inl3, DASH_inst, DASH_mirr, DASH_time, DASH_rad0, DASH_rad5, DASH_rad4, DASH_rad3, DASH_rad2,
    DASH_rad1, DASH_radb, DASH_roof, DASH_whl1, DASH_whl3, DASH_hdcr, DASH_hdcM
};
enum {  /* DS:20F2 ROAD */
    ROAD_pal0 = 0, ROAD_pol0 = 4, ROAD_sa = 8, ROAD_sp = 44, ROAD_pst0 = 80, ROAD_rck = 84,
    ROAD_govr = 100, ROAD_GOVR = 101, ROAD_dgt0 = 102, ROAD_GST0 = 112, ROAD_gst0 = 117,
    ROAD_SMK0 = 122, ROAD_smk0 = 125, ROAD_tick = 131
};

/* mulhi on a value whose sign is taken from the flags of the preceding operation:
 * `jge` → mul, else neg / mul / neg dx. */
static inline u16 mulhi_ge(bool ge, u16 ax, u16 table)
{
    if (ge) return (u16)(((u32)ax * table) >> 16);
    ax = (u16)-ax;
    return (u16)-(u16)(((u32)ax * table) >> 16);
}
/* `or ax,ax ; jge` form */
static inline u16 mulhi_s(u16 ax, u16 table) { return mulhi_ge((s16)ax >= 0, ax, table); }
/* `mul r/m ; mov al,ah ; mov ah,dl` */
static inline u16 mid16(u16 a, u16 b) { return (u16)(((u32)a * b) >> 8); }
