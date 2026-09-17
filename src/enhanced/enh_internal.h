#pragma once
/* Enhanced renderer internals (ENHANCED.md).
 *
 *   enhanced.c    hooks, simulation snapshots and extrapolation, frame orchestration, coverage, overlay
 *   enh_scene.c   the front view in continuous depth: road integrators, rows, cut lines, tunnels, and the
 *                 original drawing order turned into a display list (EnhCmd) in original coordinates
 *   enh_raster.c  sprite decoding, the indexed sample buffer, rasterisation of the display list in
 *                 horizontal bands, resolve through the palette
 *
 * Coordinates are the original's front-view buffer coordinates (320 x 92, floats); the sample buffer has
 * enh_sq samples per original pixel in each direction (output scale x supersampling). */
#include "enhanced.h"
#include "../mem.h"
#include "../symbols.h"

#define VIEW_W   320
#define VIEW_H   92
#define VIEW_Y0  19               /* screen row of the front view */

/* ---- continuous simulation state for one frame (enhanced.c) */
typedef struct {
    double s;                     /* player position in road units (unit index + sub / 256) */
    double lat;                   /* player_lateral */
    double yaw;                   /* view_yaw */
    double heading, cloud;        /* mountain / cloud scroll */
    u16 pos;                      /* DS address of the player's road byte at the last step */
    u16 counter;                  /* ring counter at the last step */
    u8 start_flags;               /* region state at the last step */
    bool frozen;                  /* no extrapolation (crash, messages) */
} EnhView;

typedef struct {                  /* a car to draw */
    double s;                     /* road position (units) */
    double lat;                   /* lateral */
    int kind;                     /* ENH_CAR_* */
    u16 type;                     /* traffic: type word */
    int order;                    /* original drawing order (tie-break) */
} EnhCar;

enum { ENH_CAR_TRAFFIC, ENH_CAR_OPP, ENH_CAR_COP, ENH_CAR_PARKED };

#define ENH_MAX_CARS 104

/* ---- display list (enh_scene.c -> enh_raster.c) */
enum { EOP_COPY, EOP_OR, EOP_AND, EOP_XOR };

typedef struct EnhSprite {
    u16 seg, off;
    int w, h, hx, hy;             /* size in pixels, hot spot */
    int ox, oy;                   /* own position */
    u8 *bits;                     /* w x h stored-plane patterns (bit k = stored plane k) */
    u8 lut[4][256];               /* [op][pattern << 4 | old colour] -> new colour */
    u16 touch[4];                 /* [op]: bit p set = pattern p changes some colour */
} EnhSprite;

enum { CMD_FILL, CMD_SPRITE, CMD_LINE, CMD_GROUND, CMD_WALLS, CMD_BAND, CMD_MARK };

typedef struct {
    u8 type, colour, op;
    float cy0, cy1;               /* clip rows [cy0, cy1) */
    float cx0, cx1;               /* clip columns [cx0, cx1) */
    float x0, y0, x1, y1;         /* FILL: rect; LINE: end points; SPRITE: top-left and scale (x1);
                                     BAND: scanline range [y0, y1); WALLS: x0 / x1 = tunnel edges,
                                     y0 / y1 = far / near end */
    float w;                      /* LINE: width */
    float alpha;                  /* < 1: dithered (fade in) */
    const EnhSprite *spr;
    int a;                        /* MARK: far row of the pair */
} EnhCmd;

/* ---- rows (enh_scene.c) */
typedef struct {
    double z, X, H;               /* depth, lateral sum, height sum */
    float cx, y, W, L, R, ol, or_, band, clip;
    int unit;                     /* road unit index */
    u8 flags;                     /* wide bit | r0 */
    u8 state;                     /* r0 state after this unit */
    u8 obj;                       /* r3 */
    u8 phase;                     /* unit counter phase (dash / pole / scenery slot) */
} EnhRow;

typedef struct {                  /* a tunnel: entrance (in) and far end (out) rows and edges */
    int in_row, out_row;
    float in_sy, in_top, out_sy, out_top, in_l, in_r, out_l, out_r;
    bool has_out;
} EnhTunnel;

#define ENH_MAX_TUNNELS 8

typedef struct {                  /* scanlines [ylo, yhi) are interpolated between rows near and far */
    float ylo, yhi;
    int near, far;
} EnhPair;

typedef struct {
    int nrows;                    /* rows 0..nrows; row j = unit (car unit + j), depth j + 3 - frac */
    EnhRow rows[ENH_MAX_ROWS + 2];
    int npairs;
    EnhPair pairs[ENH_MAX_ROWS + 2];

    /* the original's per-frame scalars (scene_render.md §3.2), rows as indices into rows[] */
    float top_sy;
    int top_row;
    float left_cut_x, right_cut_x, left_sky_x, right_sky_x, left_sky_y, right_sky_y;
    int left_cut_row, right_cut_row, left_sky_row, right_sky_row;
    u8 left_cut_state, right_cut_state;
    int tunnel_in_row, tunnel_out_row, tunnel_ceiling_row;
    float tunnel_in_sy, tunnel_in_top, tunnel_out_sy, tunnel_out_top, tunnel_ceiling;
    float tunnel_in_l, tunnel_in_r, tunnel_out_l, tunnel_out_r;
    u8 r0_state, r0_any, start_flags;
    u8 sky_state;                 /* r0 state after the nearest tunnel (the far row's state without one) */
    bool tunnel_out_found;        /* the nearest tunnel's far end is in view */
    bool tunnel_nearest;          /* the car is in a tunnel or one starts within the original's rows */
    int nextra;                   /* tunnels beyond the nearest one (the original shows at most one) */
    EnhTunnel extra[ENH_MAX_TUNNELS];
    bool style, median, backdrop_off;
    u8 col_left, col_right, col_shoulder, col_sky, col_far;

    EnhCmd *cmds;
    int ncmds, cap;
} EnhScene;

extern EnhScene enh_sc;
extern int enh_rows_setting;     /* --draw-distance */

/* enh_scene.c */
void enh_scene_build(const EnhView *v, const EnhCar *cars, int ncars);

/* enh_raster.c */
extern int enh_scale, enh_q, enh_sq;          /* output scale, supersampling, samples per pixel */
extern int enh_sw, enh_sh;                    /* sample buffer size */
extern int enh_ow, enh_oh;                    /* resolved image size (window) */
extern u32 *enh_out;                          /* resolved window image, enh_ow x enh_oh */
bool enh_raster_setup(void);                  /* (re)allocates for gfx_output_scale() */
const EnhSprite *enh_sprite(FarPtr p);        /* decoded sprite (NULL for a null / invalid handle) */
void enh_sprite_cache_clear(void);
void enh_raster_render(void);                 /* rasterises enh_sc into the sample buffer */
void enh_resolve(void);                       /* sample buffer -> enh_out through the current palette */
u32  enh_resolved_palette_key(void);          /* palette the last resolve used */
u32  enh_palette_key(void);                   /* current palette */
void enh_cover_sprite(u8 *cover, int cw, int chh, const EnhSprite *s, int x, int y, int op);
