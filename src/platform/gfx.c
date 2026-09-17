/* Planar EGA graphics — port of the TD2EGA.EXE assembly graphics library in segment 06c9
 * (port/spec/platform.md §2.1, §4.1–4.5, §4.12; scene_render.md for 06c9:5c98 / 5ca6).
 *
 * Every routine is transcribed from the disassembly (work/platform/dis_06c9.txt). Bytes are read and
 * written through vrd()/vwr() (gfx_ega.h), which address mem[] for RAM segments and the EGA hardware
 * model for segment 0xA000. All target / descriptor state stays in code-segment memory (CS:AF3A live
 * copy, CS:AF54 screen descriptor, CS:AF6E screen row table, CS:B269 pool pointer, CS:B26B pool); it is
 * statically initialised in the load image. The sprite blitters are in gfx_blit.c. */
#include "gfx_ega.h"
#include "res.h"
#include "../host.h"

#include <string.h>

EgaState gfx_ega;

void gfx_ega_reset_registers(void)
{
    gfx_ega.map_mask = 0x0F;
    gfx_ega.set_reset = 0;
    gfx_ega.enable_sr = 0;
    gfx_ega.func = 0;
    gfx_ega.read_map = 0;
    gfx_ega.mode = 0;
    gfx_ega.bit_mask = 0xFF;
}

/* ------------------------------------------------------------------------------------------------ */
/* Port setup / presentation                                                                        */

/* INT 10h mode 0Dh default palette (standard 16 colours, 200-line attribute values). */
static const u8 mode0d_default_palette[17] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x00
};

void gfx_init(void)
{
    memset(gfx_ega.plane, 0, sizeof gfx_ega.plane);
    memset(gfx_ega.latch, 0, sizeof gfx_ega.latch);
    gfx_ega_reset_registers();
    memcpy(gfx_ega.palette, mode0d_default_palette, sizeof gfx_ega.palette);
    gfx_ega.dirty = true;
    /* CS:AF3A (live copy = screen), CS:AF54 screen descriptor, CS:AF6E row table y*40, CS:B269 = B26B
     * are statically initialised in the load image; check that the image is the expected one. */
    if (CSW(GFX_SCREEN_DESC_OFF + 2) != VRAM_SEG || CSW(GFX_SCREEN_DESC_OFF + 0x0A) != CS_gfx_screen_rows
        || CSW(CS_gfx_screen_rows + 2 * 199) != 199 * 40 || CSW(CS_gfx_rowpool_top) != CS_gfx_rowpool)
        host_fatal("gfx_init: unexpected graphics state in %s", TD_EXE_NAME);
    host_set_frame_source(gfx_compose, 320, 200);
}

const u8 *gfx_vram_plane(int k) { return gfx_ega.plane[k & 3]; }

static u32 ega_rgb(u8 v)
{
    /* 200-line EGA monitor decoding: bit0 B, bit1 G, bit2 R, bit4 intensity; R+G without intensity = brown. */
    u32 r = (v & 4) ? 0xAA : 0, g = (v & 2) ? 0xAA : 0, b = (v & 1) ? 0xAA : 0;
    if ((v & 0x17) == 0x06) g = 0x55;
    if (v & 0x10) { r += 0x55; g += 0x55; b += 0x55; }
    return r << 16 | g << 8 | b;
}

bool gfx_compose(u32 *xrgb)
{
    if (!gfx_ega.dirty) return false;
    gfx_ega.dirty = false;
    u32 pal[16];
    for (int i = 0; i < 16; i++) pal[i] = ega_rgb(gfx_ega.palette[i]);
    for (int y = 0; y < 200; y++) {
        for (int bx = 0; bx < 40; bx++) {
            u32 off = (u32)(y * 40 + bx);
            u8 p0 = gfx_ega.plane[0][off], p1 = gfx_ega.plane[1][off];
            u8 p2 = gfx_ega.plane[2][off], p3 = gfx_ega.plane[3][off];
            for (int b = 0; b < 8; b++) {
                int s = 7 - b;
                int idx = (p0 >> s & 1) | (p1 >> s & 1) << 1 | (p2 >> s & 1) << 2 | (p3 >> s & 1) << 3;
                xrgb[y * 320 + bx * 8 + b] = pal[idx];
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ */
/* Video mode, palette                                                                              */

static void bios_equipment(u16 bits)
{
    /* PORT: BIOS data area 0040:0010 is not part of the port's memory model; the write is dropped. */
    (void)bits;
}

/* 06c9:5d00 gfx_video_hook: empty retf in the EGA build (TD2CGA restores its palette here). */
void gfx_video_hook(void) {}

/* 06c9:abfa gfx_set_palette: INT 10h AX=1002h, ES:DX = DS:table (16 palette registers + overscan) */
void gfx_set_palette(u16 ds_table)
{
    memcpy(gfx_ega.palette, mp(DGROUP, ds_table), sizeof gfx_ega.palette);
    gfx_ega.dirty = true;
}

/* 06c9:90e8 gfx_init_ega */
void gfx_init_ega(void)
{
    gc_out(5, 0);
    gc_out(1, 0);
    gc_out(8, 0xFF);
    gc_out(3, 0);
    gfx_clear_screen(0);
    bios_equipment(0x10);
    /* INT 10h AX=000Dh: the mode set clears video memory and loads the default register state and
     * palette. PORT: no window / renderer change; modelled as clear + register reset. */
    memset(gfx_ega.plane, 0, sizeof gfx_ega.plane);
    gfx_ega_reset_registers();
    memcpy(gfx_ega.palette, mode0d_default_palette, sizeof gfx_ega.palette);
    gfx_ega.dirty = true;
    gfx_set_palette(DS_pal_game);
}

/* 06c9:642e gfx_shutdown */
void gfx_shutdown(void)
{
    gfx_clear_screen(0);
    bios_equipment(0x10);
    /* PORT: INT 10h AX=0003h (text mode) and AH=0Bh (black border) have no host equivalent; the host
     * window is closed by host_shutdown(). Modelled as clear + register reset. */
    memset(gfx_ega.plane, 0, sizeof gfx_ega.plane);
    gfx_ega_reset_registers();
    gfx_ega.dirty = true;
}

/* 06c9:5f64 herc_init. PORT: the Hercules hardware is dropped (TD2EGA never draws on it: all
 * primitives still write A000h, and DUEL.EXE starts Hercules through td2cga.exe). Only the flag that
 * video_shutdown tests is kept; the screen stays the EGA model. */
void herc_init(void)
{
    DSB(DS_herc_mode) = 1;
    bios_equipment(0x20);
}

/* 06c9:5fbc video_shutdown */
void video_shutdown(void)
{
    if (DSB(DS_herc_mode) == 0) {
        gfx_shutdown();
        return;
    }
    /* PORT: Hercules text mode restore (3BFh, 3B8h, 6845 from DS:5E7C, clear B800h, INT 10h mode 7)
     * dropped. */
    bios_equipment(0x30);
}

/* ------------------------------------------------------------------------------------------------ */
/* Targets and buffers                                                                              */

/* 06c9:5d9e gfx_set_clip: writes the descriptor, and the live copy when plane 0 matches */
void gfx_set_clip(FarPtr desc, s16 x0, s16 x1, s16 y0, s16 y1)
{
    bool cur = CUR_PLANE(0) == rd16(desc.seg, (u16)(desc.off + 2));
    wr16(desc.seg, (u16)(desc.off + 0x0C), (u16)x0);
    wr16(desc.seg, (u16)(desc.off + 0x0E), (u16)x1);
    wr16(desc.seg, (u16)(desc.off + 0x10), (u16)y0);
    wr16(desc.seg, (u16)(desc.off + 0x12), (u16)y1);
    if (cur) {
        CUR_X0 = (u16)x0;
        CUR_X1 = (u16)x1;
        CUR_Y0 = (u16)y0;
        CUR_Y1 = (u16)y1;
    }
}

/* 06c9:5dfb gfx_set_clip_current: live copy only */
void gfx_set_clip_current(s16 x0, s16 x1, s16 y0, s16 y1)
{
    CUR_X0 = (u16)x0;
    CUR_X1 = (u16)x1;
    CUR_Y0 = (u16)y0;
    CUR_Y1 = (u16)y1;
}

/* 06c9:7d5e gfx_select_target: 13 words -> CS:AF3A */
void gfx_select_target(FarPtr desc)
{
    for (u16 i = 0; i < GFX_DESC_WORDS; i++)
        CSW((u16)(GFX_CUR_OFF + 2 * i)) = rd16(desc.seg, (u16)(desc.off + 2 * i));
}

/* 06c9:5c98 select_screen */
void select_screen(void)
{
    gfx_select_target(gfx_screen_desc());
}

/* 06c9:78f8 / 06c9:7918: 26 words CS:AF3A.. <-> DS:buf */
void gfx_targets_save(u16 dst_ds)
{
    for (u16 i = 0; i < GFX_SAVE_WORDS; i++)
        DSW((u16)(dst_ds + 2 * i)) = CSW((u16)(GFX_CUR_OFF + 2 * i));
}

void gfx_targets_restore(u16 src_ds)
{
    for (u16 i = 0; i < GFX_SAVE_WORDS; i++)
        CSW((u16)(GFX_CUR_OFF + 2 * i)) = DSW((u16)(src_ds + 2 * i));
}

/* 06c9:b0fe gfx_create_buffer */
FarPtr gfx_create_buffer(u16 width_px, u16 h, u16 plane_mask)
{
    u16 w = (u16)(width_px >> 3);                                  /* bp-12 */
    u16 size = (u16)((u32)w * h);                                  /* mul bx (low word) */
    u16 pad = (u16)(-(size & 0x0F)) & 0x0F;                        /* bp-10 */
    u16 blk = (u16)(size + pad);                                   /* bp-0C */
    u32 total = 0;                                                 /* BH:DX, 24 bits */
    for (int k = 0; k < 4; k++)
        if (plane_mask >> k & 1) total += blk;
    total += 0x10;
    u16 paras = (u16)((u16)(total >> 4) + 1);
    FarPtr blkp = mem_reserve(DS_s_window, paras);                /* "WINDOW"; only DX (segment) is used */
    u16 seg = blkp.seg;

    wr16(seg, 0, w);
    wr16(seg, 2, h);
    wr16(seg, 4, 0); wr16(seg, 6, 0); wr16(seg, 8, 0); wr16(seg, 0x0A, 0);
    for (u16 i = 0; i < 4; i++) wr8(seg, (u16)(0x0C + i), 0);
    u16 di = 0x0C;
    for (u16 bit = 1; bit <= 8; bit <<= 1)
        if (plane_mask & bit) wr8(seg, di++, (u8)(plane_mask & bit));
    wr8(seg, 0x0F, (u8)(rd8(seg, 0x0F) | (u8)(pad << 4)));

    u16 bx = CSW(CS_gfx_rowpool_top);                              /* bp-0A */
    u16 next = (u16)(((u16)(h + 0x0D) << 1) + bx);
    if (next >= 0xBA3B) fatal("%s", (const char *)mp(DGROUP, 0x68B7));   /* "OUT OF ROW TABLE SPACE" */
    CSW(CS_gfx_rowpool_top) = next;
    CSW(bx) = 0;                                                   /* hdr_off */
    u16 ax = seg, step = (u16)(blk >> 4), m = plane_mask;
    for (u16 k = 0; k < 4; k++, m >>= 1) {
        CSW((u16)(bx + 2 + 2 * k)) = 0;
        if (m & 1) {
            CSW((u16)(bx + 2 + 2 * k)) = ax;
            ax = (u16)(ax + step);
        }
    }
    CSW((u16)(bx + 0x0A)) = (u16)(bx + 0x1A);
    CSW((u16)(bx + 0x0C)) = 0;
    CSW((u16)(bx + 0x18)) = width_px;
    CSW((u16)(bx + 0x0E)) = w;
    CSW((u16)(bx + 0x14)) = w;
    CSW((u16)(bx + 0x10)) = 0;
    CSW((u16)(bx + 0x12)) = h;
    CSW((u16)(bx + 0x16)) = pad;
    u16 cnt = h, off = 0x10, b = bx;
    do {                                                           /* loop: h == 0 -> 65536 */
        CSW((u16)(b + 0x1A)) = off;
        b = (u16)(b + 2);
        off = (u16)(off + w);
    } while (--cnt);
    return far_make(ASM_SEG, bx);
}

/* 06c9:5ca6 create_buffer_like */
FarPtr create_buffer_like(FarPtr spr, FarPtr *sprite_out)
{
    u16 w = rd16(spr.seg, spr.off);
    FarPtr desc = gfx_create_buffer((u16)(w << 3), rd16(spr.seg, (u16)(spr.off + 2)), 0x0F);
    FarPtr hdr = gfx_desc_sprite(desc);
    wr16(hdr.seg, (u16)(hdr.off + 8), rd16(spr.seg, (u16)(spr.off + 8)));
    wr16(hdr.seg, (u16)(hdr.off + 0x0A), rd16(spr.seg, (u16)(spr.off + 0x0A)));
    if (sprite_out) *sprite_out = hdr;
    return desc;
}

/* 06c9:782c gfx_free_buffer: pops the descriptor pool (must be the latest buffer), frees the block */
void gfx_free_buffer(FarPtr desc)
{
    FarPtr hdr = gfx_desc_sprite(desc);
    u16 h = rd16(hdr.seg, (u16)(hdr.off + 2));
    CSW(CS_gfx_rowpool_top) = (u16)(CSW(CS_gfx_rowpool_top) - (u16)((u16)(h + 0x0D) << 1));
    mem_free(hdr);
}

/* ------------------------------------------------------------------------------------------------ */
/* Clears and fills                                                                                 */

/* 06c9:840a gfx_clear_screen: always A000h, 8000 bytes, write mode 2, map mask 0Fh */
void gfx_clear_screen(u8 colour)
{
    gc_out(5, 2);
    seq_map_mask(0x0F);
    for (u16 di = 0; di < 0x1F40; di++) ega_write(di, colour);
    gc_out(5, 0);
}

/* 06c9:843e gfx_clear_clip */
void gfx_clear_clip(u8 colour)
{
    u16 rows = (u16)(CUR_Y1 - CUR_Y0);                             /* bp-0E */
    u16 cols = (u16)(CUR_X1 - CUR_X0);                             /* bp-08 */
    u16 di0 = (u16)(row_at(CUR_Y0) + CUR_X0);                      /* bp-0A */
    u16 skip = (u16)(CUR_STRIDE - cols);                           /* bp-0C */

    if (CUR_PLANE(0) == VRAM_SEG) {
        gc_out(5, 2);
        seq_map_mask(0x0F);
        u16 di = di0;
        if (cols == 0x28) {
            u16 count = (u16)(0x28 * (u8)rows);                    /* mul dl */
            for (; count; count--) ega_write(di++, colour);
        } else {
            u16 r = rows;
            do {
                for (u16 n = cols; n; n--) ega_write(di++, colour);
                di = (u16)(di + skip);
            } while (dec_jg(&r));
        }
        gc_out(5, 0);
        return;
    }
    if (cols == CUR_STRIDE) {
        u16 total = (u16)(CUR_STRIDE * rows);                      /* imul dx (low word) */
        u16 words = (u16)(total >> 1);
        u8 c = colour;
        for (int k = 0; k < 4; k++, c >>= 1) {
            u16 seg = CUR_PLANE(k);
            if (!seg) continue;
            u8 v = (c & 1) ? 0xFF : 0x00;
            u16 di = di0;
            for (u16 n = words; n; n--) { wr8(seg, di++, v); wr8(seg, di++, v); }   /* rep stosw */
            if (total & 1) wr8(seg, di, v);
        }
        return;
    }
    u16 c = colour;
    for (int k = 0; k < 4; k++, c >>= 1) {
        u16 seg = CUR_PLANE(k);
        if (!seg) continue;
        u8 v = (c & 1) ? 0xFF : 0x00;
        u16 di = di0, r = rows;
        do {
            for (u16 n = cols; n; n--) wr8(seg, di++, v);
            di = (u16)(di + skip);
        } while (dec_jg(&r));
    }
}

/* 06c9:8a0c gfx_fill_rect (body shared with 06c9:89a2) */
static void fill_rect_body(s16 x, s16 y, s16 w, s16 h, u8 colour)
{
    if (w <= 0 || h <= 0) return;
    u16 col = (u16)(x >> 3);                                       /* bp-0C */
    u8 dh = DSB(0x6838 + (x & 7));                                 /* left mask FF 7F .. 01 */
    u16 cx = (u16)(x + w - 1);
    u8 dl = DSB(0x6840 + (cx & 7));                                /* right mask 80 C0 .. FF */
    cx = (u16)(((s16)cx >> 3) - col);                              /* last byte - first byte */
    u16 di0 = (u16)(row_at((u16)y) + col);                         /* bp-06 */
    u16 skip = (u16)(CUR_STRIDE - cx);                             /* bp-08 */
    u16 n = (u16)(cx - 1);                                         /* bp-0A */

    if (CUR_PLANE(0) != VRAM_SEG) {
        if ((s16)n > 0) {                                          /* whole middle bytes */
            u8 c = colour;
            for (int k = 0; k < 4; k++, c >>= 1) {
                u16 seg = CUR_PLANE(k);
                if (!seg) continue;
                u16 di = di0, rows = (u16)h;
                bool set = c & 1;
                u8 l = set ? dh : (u8)~dh, r = set ? dl : (u8)~dl, mid = set ? 0xFF : 0x00;
                do {
                    wr8(seg, di, set ? (u8)(rd8(seg, di) | l) : (u8)(rd8(seg, di) & l));
                    di++;
                    for (u16 m = n; m; m--) wr8(seg, di++, mid);
                    wr8(seg, di, set ? (u8)(rd8(seg, di) | r) : (u8)(rd8(seg, di) & r));
                    di = (u16)(di + skip);
                } while (dec_jg(&rows));
            }
        } else if (n == 0) {                                       /* two partial bytes */
            u8 c = colour;
            for (int k = 0; k < 4; k++, c >>= 1) {
                u16 seg = CUR_PLANE(k);
                if (!seg) continue;
                u16 di = di0, cnt = (u16)h;
                bool set = c & 1;
                do {
                    if (set) {
                        wr8(seg, di, (u8)(rd8(seg, di) | dh)); di++;
                        wr8(seg, di, (u8)(rd8(seg, di) | dl));
                    } else {
                        wr8(seg, di, (u8)(rd8(seg, di) & (u8)~dh)); di++;
                        wr8(seg, di, (u8)(rd8(seg, di) & (u8)~dl));
                    }
                    di = (u16)(di + skip);
                } while (--cnt);
            }
        } else {                                                   /* single byte */
            u8 m = dh & dl, im = (u8)~m, c = colour;
            for (int k = 0; k < 4; k++, c >>= 1) {
                u16 seg = CUR_PLANE(k);
                if (!seg) continue;
                u16 di = di0, cnt = (u16)h;
                do {
                    wr8(seg, di, (c & 1) ? (u8)(rd8(seg, di) | m) : (u8)(rd8(seg, di) & im));
                    di = (u16)(di + skip);
                } while (--cnt);
            }
        }
        return;
    }

    /* EGA: write mode 2, map mask FFh, bit mask per partial column (columns step by the live stride) */
    u8 bh = dh, bl = dl;
    u16 stride = CUR_STRIDE;
    gc_out(5, 2);
    seq_map_mask(0xFF);
    if ((s16)n > 0) {
        gc_out(8, bh);
        u16 di = di0, cnt = (u16)h;
        do { (void)ega_read(di); ega_write(di, colour); di = (u16)(di + stride); } while (--cnt);
        gc_out(8, bl);
        di = (u16)(di0 + n + 1); cnt = (u16)h;
        do { (void)ega_read(di); ega_write(di, colour); di = (u16)(di + stride); } while (--cnt);
        gc_out(8, 0xFF);
        di = (u16)(di0 + 1);
        u16 rows = (u16)h, step = (u16)(skip + 1);
        do {
            u16 m = n;
            do { (void)ega_read(di); ega_write(di, colour); di++; } while (--m);
            di = (u16)(di + step);
        } while (dec_jg(&rows));
    } else if (n == 0) {
        gc_out(8, bh);
        u16 di = di0, cnt = (u16)h;
        do { (void)ega_read(di); ega_write(di, colour); di = (u16)(di + stride); } while (--cnt);
        gc_out(8, bl);
        di = (u16)(di0 + 1); cnt = (u16)h;
        do { (void)ega_read(di); ega_write(di, colour); di = (u16)(di + stride); } while (--cnt);
    } else {
        gc_out(8, bh & bl);
        u16 di = di0, cnt = (u16)h;
        do { (void)ega_read(di); ega_write(di, colour); di = (u16)(di + stride); } while (--cnt);
    }
    gc_out(8, 0xFF);
    gc_out(5, 0);
}

void gfx_fill_rect(s16 x, s16 y, s16 w, s16 h, u8 colour)
{
    fill_rect_body(x, y, w, h, colour);
}

/* 06c9:89a2 gfx_fill_rect_clip: clip to [clip_x0*8, clip_x1*8) x [clip_y0, clip_y1), then 8a0c's body */
void gfx_fill_rect_clip(s16 x, s16 y, s16 w, s16 h, u8 colour)
{
    u16 X = (u16)x, Y = (u16)y, W = (u16)w, H = (u16)h;
    u16 cx0 = (u16)(CUR_X0 << 3);
    if (!sle(cx0, X)) {
        u16 ax = (u16)(cx0 - X);
        X = cx0;
        if (sle(W, ax)) return;
        W = (u16)(W - ax);
    }
    u16 cx1 = (u16)(CUR_X1 << 3);
    if (!sle((u16)(X + W), cx1)) {
        u16 ax = (u16)(X + W - cx1);
        if (sle(W, ax)) return;
        W = (u16)(W - ax);
    }
    if (!sle(CUR_Y0, Y)) {
        u16 ax = (u16)(CUR_Y0 - Y);
        Y = CUR_Y0;
        if (sle(H, ax)) return;
        H = (u16)(H - ax);
    }
    if (!sle((u16)(Y + H), CUR_Y1)) {
        u16 ax = (u16)(Y + H - CUR_Y1);
        if (sle(H, ax)) return;
        H = (u16)(H - ax);
    }
    fill_rect_body((s16)X, (s16)Y, (s16)W, (s16)H, colour);
}

/* 16af:0006 draw_rect_outline (note: the vertical sides are y1 - y0 rows, without the +1) */
void draw_rect_outline(s16 x0, s16 y0, s16 x1, s16 y1, u16 colour)
{
    s16 w = (s16)(x1 - x0 + 1), h = (s16)(y1 - y0);
    if (w > 0) {
        gfx_fill_rect(x0, y0, w, 1, (u8)colour);
        gfx_fill_rect(x0, y1, w, 1, (u8)colour);
    }
    if (h > 0) {
        gfx_fill_rect(x0, y0, 1, h, (u8)colour);
        gfx_fill_rect(x1, y0, 1, h, (u8)colour);
    }
}

/* 06c9:ba3c gfx_plot */
void gfx_plot(s16 x, s16 y, u8 colour)
{
    u16 bx = (u16)x & 7;
    u16 di = (u16)(((u16)x >> 3) + row_at((u16)y));
    u8 bit = DSB(DS_plot_bit_tab + bx);
    if (CUR_PLANE(0) == VRAM_SEG) {
        seq_map_mask(0xFF);
        gc_out(8, bit);
        gc_out(5, 2);
        (void)ega_read(di);
        ega_write(di, colour);
        gc_out(8, 0xFF);
        gc_out(5, 0);
        return;
    }
    u8 nb = (u8)~bit, c = colour;
    for (int k = 0; k < 4; k++, c >>= 1) {
        u16 seg = CUR_PLANE(k);
        if (!seg) continue;
        if (c & 1) wr8(seg, di, (u8)(rd8(seg, di) | bit));
        else wr8(seg, di, (u8)(rd8(seg, di) & nb));
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Lines: 06c9:8582 gfx_draw_line (pixel = colour) and 06c9:c984 gfx_draw_line_or (TD1 behaviour)   */

static inline void add32(u16 *frac, u16 *ip, u16 sfrac, u16 sint)  /* add frac ; adc int */
{
    u32 f = (u32)*frac + sfrac;
    *frac = (u16)f;
    *ip = (u16)(*ip + sint + (f >> 16));
}

static void line_plot_ram(u16 y, u16 x, u16 colour, bool or_mode)
{
    u16 di = (u16)(row_at(y) + (x >> 3));
    u8 al = CSB((or_mode ? CS_line_or_bit_tab : CS_line_bit_tab) + (x & 7)), ah = (u8)~al;
    for (int k = 0; k < 4; k++, colour >>= 1) {
        u16 seg = CUR_PLANE(k);
        if (!seg) continue;
        if (or_mode) {
            wr8(seg, di, (u8)(rd8(seg, di) | al));                 /* colour ignored */
        } else {
            wr8(seg, di, (u8)(rd8(seg, di) & ah));
            if (colour & 1) wr8(seg, di, (u8)(rd8(seg, di) | al));
        }
    }
}

static void line_plot_ega(u16 y, u16 x, u8 colour, bool or_mode)
{
    u16 di = (u16)(row_at(y) + (x >> 3));
    gc_out(8, CSB((or_mode ? CS_line_or_bit_tab : CS_line_bit_tab) + (x & 7)));
    (void)ega_read(di);
    ega_write(di, colour);
}

static void ega_line_begin(bool or_mode)
{
    gc_out(5, 2);
    if (or_mode) gc_out(3, 0x10);
    seq_map_mask(0x0F);
}

static void ega_line_end(bool or_mode, bool x_major)
{
    gc_out(5, 0);
    if (or_mode && x_major) { gc_out(3, 0); gc_out(8, 0); return; }
    gc_out(8, 0);                                                  /* bit mask left at 0 (quirk) */
    if (or_mode) gc_out(3, 0);
}

static void draw_line_common(s16 x0, s16 y0, s16 x1, s16 y1, u8 colour, bool or_mode)
{
    u16 cx0 = (u16)(CUR_X0 << 3), cx1 = (u16)(CUR_X1 << 3);        /* bp-14, bp-16 */
    u16 cy0 = CUR_Y0, cy1 = CUR_Y1;
    u16 X0 = (u16)x0, Y0 = (u16)y0, X1 = (u16)x1, Y1 = (u16)y1;

    if (Y0 == Y1) {
        if (slt(Y0, cy0) || !slt(Y0, cy1)) return;
        u16 cx = (u16)(X1 - X0), bx;
        if (cx & 0x8000) { cx = (u16)-cx; bx = X1; } else bx = X0;
        cx++;
        if (slt(bx, cx0)) {
            if (sle((u16)(cx + bx), cx0)) return;
            cx = (u16)(cx + bx - cx0);
            bx = cx0;
        }
        if (!slt(bx, cx1)) return;
        if (!sle((u16)(cx + bx), cx1)) cx = (u16)(cx - (u16)(cx + bx - cx1));
        gfx_fill_rect((s16)bx, (s16)Y0, (s16)cx, 1, colour);
        return;
    }
    if (X0 == X1) {
        if (slt(X0, cx0) || !slt(X0, cx1)) return;
        u16 cx, si;
        if (slt(Y1, Y0)) { cx = (u16)(Y0 - Y1); si = Y1; } else { cx = (u16)(Y1 - Y0); si = Y0; }
        cx++;
        if (slt(si, cy0)) {
            if (sle((u16)(cx + si), cy0)) return;
            cx = (u16)(cx + si - cy0);
            si = cy0;
        }
        if (!slt(si, cy1)) return;
        if (!sle((u16)(cx + si), cy1)) cx = (u16)(cx - (u16)(cx + si - cy1));
        gfx_fill_rect((s16)X0, (s16)si, 1, (s16)cx, colour);
        return;
    }

    u8 al = 0, ah = 0;
    u16 fx = 0, fy = 0;                                            /* bp-0A, bp-0E */
    u16 bx = (u16)(X1 - X0);
    if (bx & 0x8000) { bx = (u16)-bx; al = 1; }
    bx++;
    u16 cx = (u16)(Y1 - Y0);
    if (cx & 0x8000) { ah = 1; cx = (u16)-cx; }
    cx++;
    bool screen = CUR_PLANE(0) == VRAM_SEG;

    if (!slt(bx, cx)) {                                            /* x-major */
        u16 x, y;                                                  /* bp-08 (DX), bp-0C */
        if (al) { x = X1; y = Y1; ah ^= 1; } else { x = X0; y = Y0; }
        u16 sfrac, sint;                                           /* bp-12, bp-10 */
        if (bx == cx) {
            sfrac = 0;
            sint = ah ? 0xFFFF : 1;
        } else {
            sfrac = div32_16((u32)cx << 16, bx, NULL);
            cx = bx;
            sint = 0;
            if (ah) { sint = 0xFFFF; fy = 0xFFFF; sfrac = (u16)-sfrac; }
        }
        for (;;) {                                                 /* skip while outside */
            if (!slt(y, cy0) && slt(y, cy1) && !slt(x, cx0)) {
                if (!slt(x, cx1)) return;
                break;
            }
            x++;
            add32(&fy, &y, sfrac, sint);
            if (--cx == 0) return;
        }
        if (!screen) {
            do {
                if (slt(y, cy0) || !slt(y, cy1) || !slt(x, cx1)) return;
                line_plot_ram(y, x, colour, or_mode);
                x++;
                add32(&fy, &y, sfrac, sint);
            } while (--cx);
        } else {
            ega_line_begin(or_mode);
            do {
                if (slt(y, cy0) || !slt(y, cy1) || !slt(x, cx1)) break;
                line_plot_ega(y, x, colour, or_mode);
                x++;
                add32(&fy, &y, sfrac, sint);
            } while (--cx);
            ega_line_end(or_mode, true);
        }
        return;
    }

    /* y-major */
    u16 x, y;                                                      /* bp-08, SI */
    if (ah) { x = X1; y = Y1; al ^= 1; } else { x = X0; y = Y0; }
    u16 sfrac = div32_16((u32)bx << 16, cx, NULL), sint = 0;
    if (al) { sint = 0xFFFF; fx = 0xFFFF; sfrac = (u16)-sfrac; }
    for (;;) {
        if (!slt(x, cx0) && slt(x, cx1) && !slt(y, cy0)) {
            if (!slt(y, cy1)) return;
            break;
        }
        y++;
        add32(&fx, &x, sfrac, sint);                               /* TD1's constant-step typo is fixed */
        if (--cx == 0) return;
    }
    if (!screen) {
        do {
            if (!slt(y, cy1)) return;
            if (slt(x, cx0) || !slt(x, cx1)) return;
            line_plot_ram(y, x, colour, or_mode);
            y++;
            add32(&fx, &x, sfrac, sint);
        } while (--cx);
    } else {
        ega_line_begin(or_mode);
        do {
            if (!slt(y, cy1)) break;
            if (slt(x, cx0) || !slt(x, cx1)) break;
            line_plot_ega(y, x, colour, or_mode);
            y++;
            add32(&fx, &x, sfrac, sint);
        } while (--cx);
        ega_line_end(or_mode, false);
    }
}

void gfx_draw_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 colour)
{
    draw_line_common(x0, y0, x1, y1, colour, false);
}

void gfx_draw_line_or(s16 x0, s16 y0, s16 x1, s16 y1, u8 colour)
{
    draw_line_common(x0, y0, x1, y1, colour, true);
}

/* ------------------------------------------------------------------------------------------------ */
/* 8x8 text: 06c9:a530 / a547 (body a54f), state block DS:6820                                     */

static void text_render(const u8 *s)
{
    u8 eq = 0;                                                     /* bp-10 (screen only) */
    if (CUR_PLANE(0) == VRAM_SEG) {
        u8 fg = DSB(DS_text_fg), bg = DSB(DS_text_bg);
        eq = (u8)~(fg ^ bg);                                       /* planes where fg == bg */
        gc_out(1, eq);
        gc_out(0, fg & bg);
    }
    u16 stride = CUR_STRIDE;                                       /* bp-06 */
    if (DSW(DS_text_enabled) == 1) {
        for (;;) {
            u8 ch = *s;
            if (ch == 0) break;
            s++;
            u16 glyph = DSW((u16)(ch * 2 + DSW(DS_text_font)));   /* near ptr in DGROUP */
            if (glyph == 0) {
                if (ch == 0x0D || ch == 0x0A) {
                    DSW(DS_text_x) = DSW(DS_text_margin_x);
                    DSW(DS_text_y) = (u16)(DSW(DS_text_y) + DSW(DS_text_adv_y));
                }
                continue;
            }
            u16 pos = (u16)((DSW(DS_text_x) >> 3) + row_at(DSW(DS_text_y)));
            if (CUR_PLANE(0) != VRAM_SEG) {
                u16 dx = (u16)(DSB(DS_text_fg) | (u8)(DSB(DS_text_bg) ^ DSB(DS_text_fg)) << 8);
                for (int k = 0; k < 4; k++, dx >>= 1) {
                    u16 seg = CUR_PLANE(k);
                    if (!seg) continue;
                    u16 cnt = DSW(DS_text_glyph_h), di = pos, si = glyph;
                    if (dx & 0x100) {
                        if (dx & 1) do { wr8(seg, di, rd8(DGROUP, si++)); di = (u16)(di + stride); } while (--cnt);
                        else do { wr8(seg, di, (u8)~rd8(DGROUP, si++)); di = (u16)(di + stride); } while (--cnt);
                    } else {
                        u8 v = (dx & 1) ? 0xFF : 0x00;
                        do { wr8(seg, di, v); di = (u16)(di + stride); } while (--cnt);
                    }
                }
            } else {
                seq_map_mask((u8)(DSB(DS_text_fg) | eq));
                u16 di = pos, cnt = DSW(DS_text_glyph_h), si = glyph;
                do { u8 v = rd8(DGROUP, si++); (void)ega_read(di); ega_write(di, v); di = (u16)(di + stride); } while (--cnt);
                u8 m = (u8)((DSB(DS_text_fg) ^ DSB(DS_text_bg)) & DSB(DS_text_bg));
                if (m) {
                    seq_map_mask((u8)(DSB(DS_text_bg) | eq));
                    di = pos; cnt = DSW(DS_text_glyph_h); si = glyph;
                    do { u8 v = (u8)~rd8(DGROUP, si++); (void)ega_read(di); ega_write(di, v); di = (u16)(di + stride); } while (--cnt);
                }
            }
            DSW(DS_text_x) = (u16)(DSW(DS_text_x) + DSW(DS_text_adv_x));
        }
    }
    gc_out(1, 0);
}

void gfx_draw_text(u16 s_ds, u16 x, u16 y)
{
    DSW(DS_text_x) = x;
    DSW(DS_text_y) = y;
    text_render(mp(DGROUP, s_ds));
}

void gfx_draw_text_at_cursor(u16 s_ds) { text_render(mp(DGROUP, s_ds)); }

void gfx_draw_text_str(const char *s, u16 x, u16 y)
{
    DSW(DS_text_x) = x;
    DSW(DS_text_y) = y;
    text_render((const u8 *)s);
}

void gfx_draw_text_at_cursor_str(const char *s) { text_render((const u8 *)s); }

/* 06c9:7a82 */
void gfx_set_text_colours(u16 fg, u16 bg)
{
    DSW(DS_text_bg) = bg;
    DSW(DS_text_fg) = fg;
}

/* 06c9:7a93 (no callers) */
void gfx_set_text_cursor(u16 x, u16 y)
{
    DSW(DS_text_x) = x;
    DSW(DS_text_y) = y;
}

/* 06c9:7aa4 / 06c9:7abd: 11 words DS:6820.. <-> DS:buf */
void text_state_save(u16 buf_ds)
{
    for (u16 i = 0; i < TEXT_STATE_WORDS; i++) DSW((u16)(buf_ds + 2 * i)) = DSW((u16)(DS_text_fg + 2 * i));
}

void text_state_restore(u16 buf_ds)
{
    for (u16 i = 0; i < TEXT_STATE_WORDS; i++) DSW((u16)(DS_text_fg + 2 * i)) = DSW((u16)(buf_ds + 2 * i));
}

/* 16ab:000c draw_text_centered: x = (s16)target_width / 2 - strlen * 4 (width = CS:AF52) */
static void text_centered(const u8 *s, u16 y)
{
    u16 len = (u16)strlen((const char *)s);
    s16 half = (s16)((s16)CSW(GFX_CUR_WIDTH) / 2);                 /* cwd ; sub ax,dx ; sar ax,1 */
    DSW(DS_text_x) = (u16)(half - (s16)(u16)(len << 2));
    DSW(DS_text_y) = y;
    text_render(s);
}

void draw_text_centered(u16 s_ds, u16 y) { text_centered(mp(DGROUP, s_ds), y); }
void draw_text_centered_str(const char *s, u16 y) { text_centered((const u8 *)s, y); }

/* ------------------------------------------------------------------------------------------------ */
/* Dissolves: 06c9:8cd8 (4 phases, masks CS:8C84) and 06c9:8f12 (8 phases, masks CS:8EBA)          */

enum { DIS_COPY_LAST, DIS_COPY, DIS_CLEAR, DIS_SET };

typedef struct {
    u16 row_order;      /* CS: 12 byte row offsets */
    u16 masks;          /* CS: phase masks */
    u16 phase_and;      /* 3 or 7 */
    u16 entry_tab;      /* CS: word per nibble: AL = lowest bit value, AH = its index */
    u16 routine_tab;    /* CS: routine per nibble */
    u16 copy_last;      /* routine addresses */
} DissolveTables;

static const DissolveTables dis4 = { 0x8C78, 0x8C84, 3, 0x8C98, 0x8CB8, 0x8E2A };
static const DissolveTables dis8 = { 0x8EAE, 0x8EBA, 7, 0x8ED2, 0x8EF2, 0x9064 };

static void dissolve(FarPtr spr, u8 phase, const DissolveTables *t)
{
    u16 ds = spr.seg, so = spr.off;
    u16 es = CUR_PLANE(0);
    u16 hx = rd16(ds, (u16)(so + 8));                              /* bp-14 */
    u16 w = rd16(ds, so);                                          /* bp-16 */
    u16 step12 = (u16)((u8)w * 12);                                /* bp-0A: mul bl */
    u16 psize = (u16)((u8)rd16(ds, (u16)(so + 2)) * rd8(ds, so));  /* bp-32: mul byte ptr [si] */
    u16 adv = (u16)(psize - w);                                    /* bp-1A */

    /* TODO(verify): more than 4 entries overrun the original's frame slots (bp-28.., bp-30..); shipped
     * dissolve sprites have at most 4. Extra entries are kept here instead. */
    u16 emm[16], ert[16];
    int n = 0;
    u16 cx = 0;
    u16 p = (u16)(so + 0x0C);
    for (;;) {
        u16 bx = rd8(ds, p) & 0x0F;
        if (!bx) break;
        cx = (u16)(cx + psize);
        while (bx) {
            u16 ax = CSW((u16)(t->entry_tab + bx * 2));
            u16 rt = CSW((u16)(t->routine_tab + bx * 2)) == t->copy_last ? DIS_COPY_LAST : DIS_COPY;
            if (n < 16) { emm[n] = ax; ert[n] = rt; }
            n++;
            bx &= (u16)~ax;
        }
        if (n >= 4) break;
        p++;
    }
    u16 rewind = (u16)(cx - step12);                               /* bp-1C */
    for (int pmk = 0; pmk < 2; pmk++) {                            /* pm[0] >> 4: clear, pm[1] >> 4: set */
        u16 bx = (rd8(ds, (u16)(so + 0x0C + pmk)) >> 4) & 0x0F;
        while (bx) {
            u16 ax = CSW((u16)(t->entry_tab + bx * 2));
            if (n < 16) { emm[n] = ax; ert[n] = pmk ? DIS_SET : DIS_CLEAR; }
            n++;
            bx &= (u16)~ax;
        }
    }
    if (n > 16) n = 16;

    u16 rt = (u16)(rd16(ds, (u16)(so + 0x0A)) * 2 + CUR_ROWTAB);   /* bp-0E */
    u16 h = rd16(ds, (u16)(so + 2));
    u16 rend = (u16)(rt + h + h);                                  /* bp-10 */
    u16 data = (u16)(so + 0x10);                                   /* bp-12 */

    for (s16 pass = 11; pass >= 0; pass--) {
        u8 r0 = CSB(t->row_order + pass);
        u16 si = (u16)(data + (u16)((u8)w * r0));
        u16 rp = (u16)(r0 * 2 + rt);                               /* bp-0C */
        while (rp < rend) {
            u16 d0 = (u16)(CSW(rp) + hx);                          /* bp-1E: byte column = header x */
            u8 ah = CSB(t->masks + (phase & t->phase_and));
            for (int e = 0; e < n; e++) {
                seq_map_mask((u8)emm[e]);
                gc_out(4, (u8)(emm[e] >> 8));
                u16 c = w, di = d0;
                u16 r = ert[e];
                bool body = true, rewind_w = true;
                if (r == DIS_CLEAR) {                              /* 06c9:8e67 */
                    ah = (u8)~ah;
                    u8 dl = (u8)(vrd(es, di) & ah);
                    vwr(es, di, dl); di++;
                    ah = ror8(ah, 1);
                    if (--c == 0) body = rewind_w = false;
                } else if (r == DIS_SET) {                         /* 06c9:8e7c */
                    u8 al = ah;
                    ah = (u8)~ah;
                    u8 dl = (u8)(vrd(es, di) & ah);
                    vwr(es, di, (u8)(al | dl)); di++;
                    ah = (u8)~ah;
                    ah = ror8(ah, 1);
                    if (--c == 0) body = rewind_w = false;
                }
                if (body) {                                        /* continues in the copy loop (quirk) */
                    do {
                        u8 al = (u8)(rd8(ds, si++) & ah);
                        ah = (u8)~ah;
                        u8 dl = (u8)(vrd(es, di) & ah);
                        vwr(es, di, (u8)(al | dl)); di++;
                        ah = (u8)~ah;
                        ah = ror8(ah, 1);
                    } while (--c);
                }
                if (r == DIS_COPY_LAST) si = (u16)(si + adv);
                else if (rewind_w) si = (u16)(si - w);
            }
            phase++;
            rp = (u16)(rp + 0x18);
            si = (u16)(si - rewind);
        }
    }
}

void gfx_dissolve4(FarPtr spr, u8 phase) { dissolve(spr, phase, &dis4); }
void gfx_dissolve8(FarPtr spr, u8 phase) { dissolve(spr, phase, &dis8); }

/* ------------------------------------------------------------------------------------------------ */
/* 06c9:ae62 gfx_scroll_window: write-mode-1 row copy + one sprite row                              */

void gfx_scroll_window(s16 x, s16 y, s16 w, s16 h, s16 step, FarPtr spr, s16 srow)
{
    gc_out(5, 1);
    seq_map_mask(0x0F);
    u16 di = (u16)(row_at((u16)y) + ((u16)x >> 3));
    u16 si = (u16)(di + (u16)step);
    u16 bx = (u16)((u16)step - (u16)w);
    u16 dx = (u16)h;
    do {
        for (u16 c = (u16)w; c; c--) {                             /* rep movsb within A000h */
            u8 v = ega_read(si++);
            ega_write(di++, v);
        }
        si = (u16)(si + bx);
        di = (u16)(di + bx);
    } while (dec_jg(&dx));
    gc_out(5, 0);
    if (spr.seg == 0) return;

    u16 ds = spr.seg, so = spr.off;
    u16 bsz = (u16)((u8)rd16(ds, (u16)(so + 2)) * (u8)rd16(ds, so) + (rd8(ds, (u16)(so + 0x0F)) >> 4));
    u16 adv = (u16)(bsz - (u16)w);                                 /* bp-04 */
    si = (u16)(so + 0x10 + (u16)((u8)srow * (u8)w));
    u16 pm = (u16)(so + 0x0C), d0 = di;                            /* bp-06 */
    for (int k = 0; k < 4; k++, pm++) {
        u8 cl = rd8(ds, pm) & 0x0F;
        if (!cl) break;
        seq_map_mask(cl);
        di = d0;
        u16 c = (u16)w;
        do {                                                       /* loop */
            u8 v = rd8(ds, si++);
            (void)ega_read(di);
            ega_write(di++, v);
        } while (--c);
        si = (u16)(si + adv);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Screen grabs                                                                                     */

/* 06c9:c784 gfx_grab_screen: screen (read map, screen row table CS:[AF5E], stride 40) -> live target */
void gfx_grab_screen(s16 sx, s16 sy, s16 dx, s16 dy, s16 w_px, s16 h)
{
    u16 w = (u16)(w_px >> 3);                                      /* bp-10 */
    u16 sxb = (u16)(sx >> 3), dxb = (u16)(dx >> 3);
    u16 src0 = (u16)(CSW((u16)(sy * 2 + CSW(GFX_SCREEN_DESC_OFF + 0x0A))) + sxb);   /* bp-08 */
    u16 sskip = (u16)(0x28 - w);                                   /* bp-0C */
    u16 dst0 = (u16)(row_at((u16)dy) + dxb);                       /* bp-0A */
    u16 dskip = (u16)(CUR_STRIDE - w);                             /* bp-0E */
    for (u16 bx = 0; bx < 8; bx += 2) {
        gc_out(4, (u8)(bx >> 1));
        u16 seg = CSW((u16)(GFX_CUR_PLANE0 + bx));
        if (!seg) continue;
        u16 si = src0, di = dst0, rows = (u16)h;
        do {
            for (u16 c = w; c; c--) { u8 v = ega_read(si++); vwr(seg, di++, v); }   /* rep movsb */
            si = (u16)(si + sskip);
            di = (u16)(di + dskip);
        } while (dec_jg(&rows));
    }
}

/* 06c9:ac1c / ac40 / ac64 grab_into_sprite: screen -> the sprite's stored planes. The source position
 * uses the live target's row table and stride (the screen when the screen is selected). */
static void grab_into_sprite(FarPtr spr, u16 px, u16 py)
{
    u16 ds = spr.seg, so = spr.off;
    u16 w = rd16(ds, so), h = rd16(ds, (u16)(so + 2));             /* bp-0C, bp-0E */
    u16 src0 = (u16)(row_at(py) + (u16)((s16)px >> 3));            /* bp-10 */
    u16 sskip = (u16)(CUR_STRIDE - w);                             /* bp-12 */
    u16 pad = rd8(ds, (u16)(so + 0x0F)) >> 4;                      /* bp-18 */
    u16 pm = (u16)(so + 0x0C), di = (u16)(so + 0x10);
    for (int k = 0; k < 4; k++, pm++) {
        u16 bx = rd16(ds, pm) & 0x0F;
        if (bx) {
            gc_out(4, CSB(CS_grab_readmap_tab + bx));
            u16 si = src0, rows = h;
            do {
                for (u16 c = w; c; c--) wr8(ds, di++, ega_read(si++));   /* rep movsb */
                si = (u16)(si + sskip);
            } while (dec_jg(&rows));
        }
        di = (u16)(di + pad);
    }
}

void grab_into_sprite_hot(FarPtr spr, s16 x, s16 y)
{
    grab_into_sprite(spr, (u16)((u16)x - rd16(spr.seg, (u16)(spr.off + 4))),
                     (u16)((u16)y - rd16(spr.seg, (u16)(spr.off + 6))));
}

void grab_into_sprite_raw(FarPtr spr, s16 x, s16 y)
{
    wr16(spr.seg, (u16)(spr.off + 8), (u16)x);
    wr16(spr.seg, (u16)(spr.off + 0x0A), (u16)y);
    grab_into_sprite(spr, (u16)x, (u16)y);
}

void grab_into_sprite_own(FarPtr spr)
{
    grab_into_sprite(spr, (u16)(rd16(spr.seg, (u16)(spr.off + 8)) & 0xFFF8),
                     rd16(spr.seg, (u16)(spr.off + 0x0A)));
}
