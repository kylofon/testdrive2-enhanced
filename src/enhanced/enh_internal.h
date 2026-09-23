#pragma once
/* Enhanced renderer internals (ENHANCED.md).
 *
 *   enhanced.c    hooks, simulation snapshots and extrapolation, frame orchestration, coverage, overlay
 *   enh_scene.c   the front view and the mirror in continuous depth: road integrators, rows, cut lines,
 *                 tunnels, and the original drawing order turned into a display list (EnhCmd) in original
 *                 coordinates
 *   enh_raster.c  sprite decoding, the indexed sample buffers, rasterisation of the display lists in
 *                 horizontal bands, resolve through the palette
 *
 * Coordinates are the original's view buffer coordinates (front 320 x 92, mirror 80 x 17, floats); a sample
 * buffer has enh_sq samples per original pixel in each direction (output scale x supersampling). */
#include "enhanced.h"
#include "../mem.h"
#include "../symbols.h"

#define VIEW_W   320
#define VIEW_H   92
#define VIEW_Y0  19               /* screen row of the front view */
#define MIRROR_W 80
#define MIRROR_H 17
#define ENH_MIRROR_ROWS(front_rows) ((front_rows) * 25 / 60)   /* mirror rows for a front distance */

/* ---- continuous simulation state for one frame (enhanced.c) */
typedef struct {
    double s;                     /* player position in road units (unit index + sub / 256) */
    double lat;                   /* player_lateral (at s, for the car list and the trace) */
    double yaw;                   /* view_yaw (at s, for the mountains and the trace) */
    double yaw_a, yaw_b;          /* view_yaw of the car's unit and of the next one (the two blended views) */
    double lat_a, lat_b;          /* player_lateral of the same two views */
    double heading, cloud;        /* mountain / cloud scroll */
    u16 pos;                      /* DS address of the player's road byte at the last step */
    u16 counter;                  /* ring counter at the last step */
    u8 start_flags;               /* region state at the last step */
    u8 fall_mode;                 /* falling off the road: 1 left, 2 right, 4 water */
    double fall_v;                /* fall_scroll */
    double fall_drop;             /* falling: the eye this far below the road (height units), from fall_scroll */
    bool frozen;                  /* no extrapolation (crash, messages) */
} EnhView;

typedef struct {                  /* a car to draw */
    double s;                     /* road position (units) */
    double lat;                   /* lateral */
    int kind;                     /* ENH_CAR_* */
    u16 type;                     /* traffic: type word */
    int order;                    /* original drawing order (tie-break) */
    int id;                       /* identity across frames (trace) */
} EnhCar;

enum { ENH_CAR_TRAFFIC, ENH_CAR_OPP, ENH_CAR_COP, ENH_CAR_PARKED };

#define ENH_MAX_CARS 104

/* ---- colours. The sample buffers hold palette indices: 0..15 are the EGA palette registers (as displayed,
 * so the crash flash applies), 16.. are extended colours, each a mix of palette registers in linear light
 * (so they follow the palette too) with an EGA colour of its own that sprite operations and the road
 * markings see (enh_base). Extended colours come in ramps: a ramp of n levels from one colour to another,
 * level 0 = the first colour. */
#define ENH_HAZE_COL 0xFF          /* EnhMix.c: the haze colour (the sky colour, white and light grey) */
typedef struct {
    u8 a, b, c;                   /* linear: ((1 - t) a + t b) * k, then mixed towards c by h */
    float t, k, h;
} EnhMix;

enum {                            /* extended colour ramps (enh_raster.c enh_colours_setup) */
    EXT_ROAD = 16,                /* road surface, the original's colour 7 -> the alternate shade (ENH_SHADES) */
    EXT_SHLD = EXT_ROAD + 8,      /* shoulder colour -> its alternate shade */
    EXT_MARK_C = EXT_SHLD + 8,    /* road colour -> centre line (14), by coverage (ENH_COVER levels) */
    EXT_MARK_L = EXT_MARK_C + 8,  /* road colour -> lane line (15) */
    EXT_ROCK = EXT_MARK_L + 8,    /* rock face (6) -> hazed (ENH_HAZE levels) */
    EXT_DROP = EXT_ROCK + 16,     /* rock face below a drop-off edge (shaded rock): [gradient 0..3][haze 0..7] */
    EXT_VALLEY = EXT_DROP + 32,   /* valley floor: [haze 0..7][texture 0..7: wood, then fields] */
    EXT_VOID = EXT_VALLEY + 64,   /* the drop-off side above the valley's horizon, and all of it with --valley off
                                     (the original's sky colour) */
    EXT_FENCE = EXT_VOID + 1,     /* bridge fence: colour 8 -> hazed (8 levels) */
    EXT_ROCK_END = EXT_FENCE + 8,  /* the farthest rock (fully hazed) -> the sky colour (ENH_ROCK_END levels) */
    EXT_SIDE_L = EXT_ROCK_END + 8, /* ground left of the road -> its alternate shade (ENH_SHADES; --enhanced-sides) */
    EXT_SIDE_R = EXT_SIDE_L + 8,  /* ground right of the road -> its alternate shade */
    EXT_END
};
#define ENH_SHADES 8
#define ENH_COVER  8
#define ENH_HAZE   16
#define ENH_ROCK_END 8
#define ENH_ROCK_FADE 0.12        /* rock turns into the sky colour over this share of the drawn distance */
#define ENH_NCOL   256

#define CLIFF_LEAN 0.2            /* slant of a rock face: px outwards per px up (clfo / rcfa) */

extern u8 enh_base[ENH_NCOL];    /* EGA colour of each index */
extern u8 enh_void[ENH_NCOL];    /* 1: the drop-off side (void, valley, rim, hillside): rims and hillsides
                                    only paint over these */
void enh_colours_setup(u8 col_left, u8 col_right, u8 col_shoulder, u8 col_sky);

/* ---- display list (enh_scene.c -> enh_raster.c) */
enum { EOP_COPY, EOP_OR, EOP_AND, EOP_XOR };

#define ENH_MIPS 6

typedef struct EnhSprite {
    u16 seg, off;
    int w, h, hx, hy;             /* size in pixels, hot spot */
    int ox, oy;                   /* own position */
    u8 *bits;                     /* w x h stored-plane patterns (bit k = stored plane k) */
    u8 lut[4][256];               /* [op][pattern << 4 | old colour] -> new colour */
    u16 touch[4];                 /* [op]: bit p set = pattern p changes some colour */
    u8 green;                   /* scenery: 0 not known yet, 1 no, 2 a tree or shrub (enh_scene.c) */
    u8 detail_ref;              /* scenery group (on variant 0's mask): 0 not known yet, else 1 + its largest
                                   variant that is not cropped at the top (enh_scene.c scenery_ref) */
    u8 shape;                   /* group (on variant 0): 0 not known yet, 1 one shape at every size, 2 framed
                                   differently at different sizes (enh_scene.c shape_varies) */
    /* reduced copies for strong downscaling: level L (1..nmip) has texels of 2^L x 2^L source pixels, each
     * the most frequent pattern that changes something under op and the share of such pixels (0..255) */
    int nmip;
    int mip_w[ENH_MIPS + 1], mip_h[ENH_MIPS + 1], mip_off[ENH_MIPS + 1];
    u8 *mip[4];                   /* [op]: pattern, coverage pairs of all levels */
} EnhSprite;

enum { CMD_FILL, CMD_SPRITE, CMD_LINE, CMD_GROUND, CMD_WALLS, CMD_BAND, CMD_MARK, CMD_FACE, CMD_DROP, CMD_FENCE, CMD_HILL };

typedef struct {
    u8 type, colour, op;
    u8 noshift;                   /* window coordinates (not scrolled with the view when falling) */
    float cy0, cy1;               /* clip rows [cy0, cy1) */
    float cx0, cx1;               /* clip columns [cx0, cx1) */
    float x0, y0, x1, y1;         /* FILL: rect; LINE: end points; SPRITE: top-left and scale (x1);
                                     BAND: scanline range [y0, y1); WALLS: x0 / x1 = tunnel edges,
                                     y0 / y1 = far / near end; FACE: x0 / x1 = height of the far / near
                                     row's face (px) */
    float w;                      /* LINE: width; FACE: 1 = the nearest face, covering everything outwards */
    float alpha;                  /* < 1: dithered (fade in) */
    const EnhSprite *spr;
    const EnhSprite *spr2;        /* SPRITE: an image applied with OR after spr (op AND) in one pass, and only
                                     on rock (the cliff decorations: they stay on the rock face) */
    int a;                        /* MARK, FACE, DROP: far row of the pair (the near row is a - 1); FACE,
                                     DROP: op = 1 left side, 0 right side; HILL: the entrance row of a far
                                     tunnel, op = the state of the row before it (its sides), x0 / x1 = the
                                     mouth's edges, y0 = its top, y1 = the clip above the road; SPRITE:
                                     > 0 a cliff decoration's row (drawn on rock only), < 0 a car's depth
                                     (compare dump) */
} EnhCmd;

/* ---- rows (enh_scene.c) */
typedef struct {
    double z, X, H;               /* depth, lateral sum, height sum */
    float cx, y, W, L, R, ol, or_, band, clip;
    float wl, wr;                 /* tunnel walls of the nearer rows: nothing of this row shows left of wl or
                                     right of wr (walls_clip) */
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
    /* the view: the front view, or the mirror (scene_render.md §4.11: walks backwards) */
    bool front;
    int vw, vh;                   /* buffer size: 320 x 92 / 80 x 17 */
    float horizon, centre;        /* row y and x offsets: 51, 125 / 8, 40 */
    double kx, ky, kw;            /* x / y / half-width scale: xs = kx * 65536 / depth etc. */
    double lat_k;                 /* lateral factor: 1 / 1/2 (the mirror halves the laterals) */
    int orig_rows, depth0;        /* the original's rows and the depth of its row 0: 60, 4 / 25, 6 */
    float cut_offset, sky_cut, portal_y;   /* 22, 15, 0x5B / 6, 3, 0x10 */
    u16 sky_handles;              /* DS:0704 / DS:27BE */
    u16 carscale;                 /* DS table of car size variants per original row */
    int band_dx;                  /* walk_ptr - 0x3B33 of a row: unit + 31 / unit + 29 */
    int scenery_rows;             /* rows the original draws scenery on: 44 / 25 */

    int nrows;                    /* rows 0..nrows; front: row j = unit (car unit + j), depth j + 3 - frac;
                                     mirror: row j = unit (car unit + 1 - j), depth j + 5 + frac */
    EnhRow rows[ENH_MAX_ROWS + 2];
    int npairs;
    EnhPair pairs[ENH_MAX_ROWS + 2];

    /* the original's per-frame scalars (scene_render.md §3.2), rows as indices into rows[] */
    float top_sy;                 /* highest road point of all rows */
    float top_sy_near;            /* highest road point of the original's rows: where it puts the horizon */
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

    float yoff;                   /* falling (water): the view is drawn scrolled up by this */
    double fall_drop;             /* falling: the eye this far below the road (0: on the road) */

    /* new-asset parameters (ENHANCED.md "New assets") */
    double u0, uk;                /* road position (units) at depth z: u0 + uk * z */
    double valley_shift;          /* the valley floor pans with the mountains (px) */

    EnhCmd *cmds;
    int ncmds, cap;
} EnhScene;

extern EnhScene enh_sc, enh_mc;  /* front view, mirror */
extern int enh_rows_setting;     /* --draw-distance */

/* enh_scene.c */
void enh_scene_build(const EnhView *v, const EnhCar *cars, int ncars);   /* front view (enh_sc) */
/* mirror (enh_mc); mirror_fall = scroll of the frozen image in the water */
void enh_mirror_build(const EnhView *v, const EnhCar *cars, int ncars, double mirror_fall);
double enh_scene_screen_x(double s_unit, double lat);   /* trace: screen x at a road position (NAN if not in view) */

/* enhanced.c: roadside scenery of a road unit as the ring held it when the car passed it (mirror) */
bool enh_scenery_at(int unit, s8 *type, s8 *offset);
/* enhanced.c: a line of text (position indicator glyphs) at the left edge, `row` screen rows down, into a
 * 320k x 200k frame */
void enh_draw_text(u32 *px, int k, int row, const char *txt);

/* enh_viewer.c: rows the map viewer adds above and below the front view (0: the game's 320 x 92 view; the
 * viewer's fills the 320 x 200 screen, the horizon enh_view_top rows lower) */
extern int enh_view_top, enh_view_bottom;

/* enh_raster.c */
#define ENH_MAX_BANDS 64
typedef struct {
    EnhScene *sc;
    int vw, vh;                   /* original size */
    int sw, sh;                   /* sample buffer size */
    int ow, oh;                   /* resolved image size */
    u8 *smp;                      /* sw x sh palette indices */
    s16 *g_near, *g_far;          /* per sample row: ground pair (-1: none) */
    float *g_t, *g_l, *g_r;       /* per sample row: interpolation, clamped road edges */
    u8 *face_id;                  /* sw x sh: the far row of the rock face that drew a sample (0: none) */
    int *tx_buf;                  /* per band: texel column of each sample column */
    int nbands, band_o0[ENH_MAX_BANDS + 1];
    u32 *out;                     /* resolved image, ow x oh */
    u32 pal_key;                  /* palette of the last resolve */
} EnhTarget;

extern EnhTarget enh_front, enh_mirror;
extern int enh_scale, enh_q, enh_sq;          /* output scale, supersampling, samples per pixel */
bool enh_raster_setup(void);                  /* (re)allocates for gfx_output_scale() */
const EnhSprite *enh_sprite(FarPtr p);        /* decoded sprite (NULL for a null / invalid handle) */
void enh_sprite_cache_clear(void);
void enh_raster_render(EnhTarget *t);         /* rasterises t->sc into the sample buffer and resolves */
void enh_resolve(EnhTarget *t);               /* sample buffer -> t->out through the current palette */
u32  enh_palette_key(void);                   /* current palette */
double enh_rock_haze(double z);                /* rock face haze at depth z, 0..1 (EXT_ROCK levels) */
double enh_rock_end(const EnhScene *S, double z);  /* 0..1: how far the rock at depth z has turned into the sky */
void enh_cover_sprite(u8 *cover, int cw, int chh, const EnhSprite *s, int x, int y, int op);
