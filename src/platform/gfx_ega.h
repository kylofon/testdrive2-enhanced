#pragma once
/* Private to the graphics module (gfx.c, gfx_blit.c): the EGA hardware model and small helpers for
 * transcribing the assembly.
 *
 * VRAM and adapter registers are host-side (not part of mem[]). The model implements the sequencer map
 * mask, the graphics controller set/reset, enable set/reset, function (replace/AND/OR/XOR), read map,
 * write modes 0/1/2 and bit mask, so the screen-path register tricks and their quirks (constant-plane
 * helpers, bit mask left at 0 after the line routines, GC function left set by an empty clipped blit,
 * inherited registers in the text renderer, ...) fall out of the transcription. Each plane holds the full
 * 64 KB the CPU can address, so 16-bit offset overruns land in invisible VRAM exactly like on the card. */
#include "gfx.h"

#define EGA_PLANE_BYTES 0x10000u

typedef struct {
    u8 plane[4][EGA_PLANE_BYTES];
    u8 latch[4];      /* last CPU read of all planes (write mode 1 source) */
    u8 map_mask;      /* sequencer index 2 */
    u8 set_reset;     /* GC index 0 */
    u8 enable_sr;     /* GC index 1 */
    u8 func;          /* GC index 3 (bits 3-4: 0 replace, 1 AND, 2 OR, 3 XOR) */
    u8 read_map;      /* GC index 4 */
    u8 mode;          /* GC index 5 (write mode in bits 0-1) */
    u8 bit_mask;      /* GC index 8 */
    u8 palette[17];   /* attribute controller palette registers + overscan (INT 10h AX=1002h) */
    bool dirty;
} EgaState;

extern EgaState gfx_ega;

void gfx_ega_reset_registers(void);

static inline void gc_out(u8 index, u8 value)      /* out 3CEh,index ; out 3CFh,value */
{
    switch (index) {
    case 0: gfx_ega.set_reset = value; break;
    case 1: gfx_ega.enable_sr = value; break;
    case 3: gfx_ega.func = value; break;
    case 4: gfx_ega.read_map = value; break;
    case 5: gfx_ega.mode = value; break;
    case 8: gfx_ega.bit_mask = value; break;
    default: break;
    }
}

static inline void seq_map_mask(u8 value) { gfx_ega.map_mask = value; }   /* out 3C4h,2 ; out 3C5h,value */

static inline u8 ega_read(u16 off)
{
    for (int k = 0; k < 4; k++) gfx_ega.latch[k] = gfx_ega.plane[k][off];
    return gfx_ega.plane[gfx_ega.read_map & 3][off];
}

static inline void ega_write(u16 off, u8 v)
{
    for (int k = 0; k < 4; k++) {
        if (!(gfx_ega.map_mask >> k & 1)) continue;
        u8 d = gfx_ega.plane[k][off];
        u8 x;
        switch (gfx_ega.mode & 3) {
        case 1:
            gfx_ega.plane[k][off] = gfx_ega.latch[k];      /* write mode 1 ignores function and bit mask */
            continue;
        case 2:
            x = (v >> k & 1) ? 0xFF : 0x00;
            break;
        default:
            x = (gfx_ega.enable_sr >> k & 1) ? ((gfx_ega.set_reset >> k & 1) ? 0xFF : 0x00) : v;
            break;
        }
        switch (gfx_ega.func >> 3 & 3) {
        case 1: x &= d; break;
        case 2: x |= d; break;
        case 3: x ^= d; break;
        default: break;
        }
        gfx_ega.plane[k][off] = (u8)((x & gfx_ega.bit_mask) | (d & (u8)~gfx_ega.bit_mask));
    }
    gfx_ega.dirty = true;
}

/* Byte access through a segment: 0xA000 = the EGA model, anything else = mem[]. */
static inline u8 vrd(u16 seg, u16 off) { return seg == VRAM_SEG ? ega_read(off) : rd8(seg, off); }
static inline void vwr(u16 seg, u16 off, u8 v)
{
    if (seg == VRAM_SEG) ega_write(off, v);
    else wr8(seg, off, v);
}

/* ---- flag helpers for the asm's signed branches */
static inline bool slt(u16 a, u16 b) { return (s16)a < (s16)b; }   /* cmp a,b ; jl  */
static inline bool sle(u16 a, u16 b) { return (s16)a <= (s16)b; }  /* cmp a,b ; jle */
static inline bool sgt(u16 a, u16 b) { return (s16)a > (s16)b; }   /* cmp a,b ; jg  */
/* dec r ; jg — taken while the true value old-1 is > 0 */
static inline bool dec_jg(u16 *r) { s16 old = (s16)*r; *r = (u16)(*r - 1); return old > 1; }
static inline u8 ror8(u8 v, unsigned n) { n &= 7; return n ? (u8)(v >> n | v << (8 - n)) : v; }

/* ---- live descriptor copy CS:AF3A */
#define CUR_PLANE(k) CSW(GFX_CUR_PLANE0 + 2 * (k))
#define CUR_ROWTAB   CSW(GFX_CUR_ROWTAB)
#define CUR_STRIDE   CSW(GFX_CUR_STRIDE)
#define CUR_X0       CSW(GFX_CUR_CLIP_X0)
#define CUR_X1       CSW(GFX_CUR_CLIP_X1)
#define CUR_Y0       CSW(GFX_CUR_CLIP_Y0)
#define CUR_Y1       CSW(GFX_CUR_CLIP_Y1)

/* Row table lookup through the live copy: CS:[CS:AF44 + y*2]. */
static inline u16 row_at(u16 y) { return CSW((u16)(y * 2 + CUR_ROWTAB)); }
