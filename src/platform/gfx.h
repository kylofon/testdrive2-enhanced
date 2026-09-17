#pragma once
/* Planar EGA graphics — port of the TD2EGA.EXE assembly graphics library in segment 06c9
 * (port/spec/platform.md §2.1, §3.1, §4.1–4.5, §4.12; scene_render.md for 06c9:5c98 / 06c9:5ca6).
 *
 * Model (platform.md §4.1): a draw *target* is the original's 13-word descriptor, living in
 * code-segment memory (the CS row pool CS:B26B..CS:BA3A, or the screen descriptor CS:AF54):
 *   +00 hdr_off, +02..+08 plane_seg[4] (0 = absent, 0xA000 = EGA screen), +0A rowtab (CS offset),
 *   +0C clip_x0, +0E clip_x1 (byte columns, x1 exclusive), +10 clip_y0, +12 clip_y1 (rows, exclusive),
 *   +14 stride (bytes per row), +16 pad, +18 width in pixels (new in TD2), +1A row table.
 * The selected target is copied to CS:AF3A..CS:AF53 (game code reads/pokes it there, e.g. the clip).
 * (hdr_off, plane_seg[0]) is the far pointer to the buffer's 16-byte sprite header.
 * Plane segment 0xA000 addresses the four EGA planes held by this module (not in mem[]); any other
 * segment is RAM in mem[]. Sprites, off-screen buffers and descriptors are FarPtrs into mem[].
 *
 * Pointer conventions (PORTING.md): far pointers are FarPtr, near DGROUP pointers are u16 DS offsets.
 * Blitters take the sprite as a FarPtr (the original pushes offset, segment).
 */
#include "../mem.h"
#include "../symbols.h"

/* ---- code-segment layout (segment 06c9 = ASM_SEG) */
#define GFX_CUR_OFF        0xAF3A      /* CS_gfx_cur: live copy of the selected descriptor (13 words) */
#define GFX_CUR_PLANE0     0xAF3C      /* CS_gfx_cur_planes */
#define GFX_CUR_ROWTAB     0xAF44      /* CS_gfx_cur_rowtab */
#define GFX_CUR_CLIP_X0    0xAF46      /* CS_gfx_cur_clip[0] (byte columns) */
#define GFX_CUR_CLIP_X1    0xAF48
#define GFX_CUR_CLIP_Y0    0xAF4A
#define GFX_CUR_CLIP_Y1    0xAF4C
#define GFX_CUR_STRIDE     0xAF4E      /* CS_gfx_cur_stride */
#define GFX_CUR_PAD        0xAF50      /* CS_gfx_cur_pad */
#define GFX_CUR_WIDTH      0xAF52      /* CS_gfx_cur_width (pixels) */
#define GFX_SCREEN_DESC_OFF 0xAF54     /* CS_gfx_screen_desc */
#define GFX_DESC_WORDS     13
#define GFX_SAVE_WORDS     26          /* gfx_targets_save/restore: live copy + screen descriptor */
#define TEXT_STATE_WORDS   11          /* text_state_save/restore: DS:6820..DS:6835 */

/* Far pointer to the screen descriptor CS:AF54 (what select_screen() selects). */
static inline FarPtr gfx_screen_desc(void) { return far_make(ASM_SEG, GFX_SCREEN_DESC_OFF); }
/* Far pointer to the sprite header of a buffer descriptor: (desc.hdr_off, desc.plane_seg[0]). */
static inline FarPtr gfx_desc_sprite(FarPtr desc)
{
    return far_make(rd16(desc.seg, (u16)(desc.off + 2)), rd16(desc.seg, desc.off));
}

/* ---- port setup / presentation (no original counterpart) */

/* Resets the EGA model (planes, registers, default mode-0Dh palette), checks the statically
 * initialised target state in CS and installs gfx_compose with host_set_frame_source(.., 320, 200).
 * Call once after mem_load_exe() and host_init(), before the game's main(). */
void gfx_init(void);
/* Fills xrgb (320x200) from the EGA planes through the current palette; returns true if VRAM or the
 * palette changed since the last call. */
bool gfx_compose(u32 *xrgb);
/* Read access to the emulated VRAM (verification tools / debugging): plane k (0..3), 64 KB each. */
const u8 *gfx_vram_plane(int k);

/* ---- video mode, palette */
void gfx_video_hook(void);                                              /* 06c9:5d00 (empty retf in EGA) */
void herc_init(void);                                                   /* 06c9:5f64 PORT: no Hercules */
void video_shutdown(void);                                              /* 06c9:5fbc */
void gfx_shutdown(void);                                                /* 06c9:642e */
void gfx_init_ega(void);                                                /* 06c9:90e8 */
void gfx_set_palette(u16 ds_table);                                     /* 06c9:abfa 17 bytes at DS:table */

/* ---- targets and buffers */
void   gfx_set_clip(FarPtr desc, s16 x0, s16 x1, s16 y0, s16 y1);       /* 06c9:5d9e */
void   gfx_set_clip_current(s16 x0, s16 x1, s16 y0, s16 y1);            /* 06c9:5dfb (live copy only) */
void   gfx_select_target(FarPtr desc);                                  /* 06c9:7d5e */
void   select_screen(void);                                             /* 06c9:5c98 select_target(CS:AF54) */
FarPtr gfx_create_buffer(u16 width_px, u16 h, u16 plane_mask);          /* 06c9:b0fe -> descriptor */
/* 06c9:5ca6: buffer of the sprite's size (w*8 x h, planes 0Fh) with the sprite's x, y copied into the
 * buffer header. Returns the descriptor (DX:AX); *sprite_out (may be NULL) = the buffer's sprite
 * header (CX:BX). */
FarPtr create_buffer_like(FarPtr spr, FarPtr *sprite_out);              /* 06c9:5ca6 */
void   gfx_free_buffer(FarPtr desc);                                    /* 06c9:782c */
void   gfx_targets_save(u16 dst_ds);                                    /* 06c9:78f8 26 words CS:AF3A -> DS:dst */
void   gfx_targets_restore(u16 src_ds);                                 /* 06c9:7918 */

/* ---- fills, clears, lines, plot */
void gfx_clear_screen(u8 colour);                                       /* 06c9:840a (always A000h) */
void gfx_clear_clip(u8 colour);                                         /* 06c9:843e */
void gfx_draw_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 colour);          /* 06c9:8582 pixel = colour */
void gfx_fill_rect_clip(s16 x, s16 y, s16 w, s16 h, u8 colour);         /* 06c9:89a2 */
void gfx_fill_rect(s16 x, s16 y, s16 w, s16 h, u8 colour);              /* 06c9:8a0c */
void gfx_plot(s16 x, s16 y, u8 colour);                                 /* 06c9:ba3c */
void gfx_draw_line_or(s16 x0, s16 y0, s16 x1, s16 y1, u8 colour);       /* 06c9:c984 pixel |= colour */

/* ---- screen effects, grabs */
void gfx_dissolve4(FarPtr spr, u8 phase);                               /* 06c9:8cd8 */
void gfx_dissolve8(FarPtr spr, u8 phase);                               /* 06c9:8f12 */
/* x in pixels, w in bytes; spr.seg == 0: no sprite row */
void gfx_scroll_window(s16 x, s16 y, s16 w, s16 h, s16 step, FarPtr spr, s16 srow);  /* 06c9:ae62 */
void gfx_grab_screen(s16 sx, s16 sy, s16 dx, s16 dy, s16 w_px, s16 h);  /* 06c9:c784 */
void grab_into_sprite_hot(FarPtr spr, s16 x, s16 y);                    /* 06c9:ac1c */
void grab_into_sprite_raw(FarPtr spr, s16 x, s16 y);                    /* 06c9:ac40 (stores x, y) */
void grab_into_sprite_own(FarPtr spr);                                  /* 06c9:ac64 */

/* ---- 8x8 text (state block DS:6820) */
void gfx_set_text_colours(u16 fg, u16 bg);                              /* 06c9:7a82 */
void gfx_set_text_cursor(u16 x, u16 y);                                 /* 06c9:7a93 */
void text_state_save(u16 buf_ds);                                       /* 06c9:7aa4 11 words -> DS:buf */
void text_state_restore(u16 buf_ds);                                    /* 06c9:7abd */
void gfx_draw_text(u16 s_ds, u16 x, u16 y);                             /* 06c9:a530 string at DS:s */
void gfx_draw_text_at_cursor(u16 s_ds);                                 /* 06c9:a547 */
/* PORT: the same entry points for a string that is not in DGROUP (e.g. a C buffer). */
void gfx_draw_text_str(const char *s, u16 x, u16 y);
void gfx_draw_text_at_cursor_str(const char *s);
void draw_cursor_glyph(s16 x, s16 y, u16 idx);                          /* 06c9:c958 */
/* Small C helpers in their own segments (platform.md §2.2), implemented here. */
void draw_text_centered(u16 s_ds, u16 y);                               /* 16ab:000c */
void draw_text_centered_str(const char *s, u16 y);                      /* PORT: C-string variant */
void draw_rect_outline(s16 x0, s16 y0, s16 x1, s16 y1, u16 colour);     /* 16af:0006 */

/* ---- sprite blitters (platform.md §4.2)
 * hot = at (x - hot_x, y - hot_y), raw = at (x, y), own = at the header's (x, y) (not masked in TD2).
 * *_clip_* clip against the live clip rectangle; the others do not clip. */
void blit_copy_clip_hot(FarPtr spr, s16 x, s16 y);                      /* 06c9:916c REPLACE clipped */
void blit_copy_clip_raw(FarPtr spr, s16 x, s16 y);                      /* 06c9:91a4 */
void blit_copy_clip_own(FarPtr spr);                                    /* 06c9:91d6 */
void blit_or_clip_hot(FarPtr spr, s16 x, s16 y);                        /* 06c9:a6d8 OR clipped */
void blit_or_clip_raw(FarPtr spr, s16 x, s16 y);                        /* 06c9:a6f8 */
void blit_or_clip_own(FarPtr spr);                                      /* 06c9:a718 */
void blit_and_clip_hot(FarPtr spr, s16 x, s16 y);                       /* 06c9:7d7c AND clipped */
void blit_and_clip_raw(FarPtr spr, s16 x, s16 y);                       /* 06c9:7d9c */
void blit_and_clip_own(FarPtr spr);                                     /* 06c9:7dbc */
void blit_xor_clip_hot(FarPtr spr, s16 x, s16 y);                       /* 06c9:bac8 XOR clipped */
void blit_xor_clip_raw(FarPtr spr, s16 x, s16 y);                       /* 06c9:bae8 */
void blit_xor_clip_own(FarPtr spr);                                     /* 06c9:bb08 */
void blit_copy_hot(FarPtr spr, s16 x, s16 y);                           /* 06c9:9c58 REPLACE unclipped */
void blit_copy_raw(FarPtr spr, s16 x, s16 y);                           /* 06c9:9c90 */
void blit_copy_own(FarPtr spr);                                         /* 06c9:9cc2 */
void blit_or_hot(FarPtr spr, s16 x, s16 y);                             /* 06c9:a9d2 OR unclipped */
void blit_or_raw(FarPtr spr, s16 x, s16 y);                             /* 06c9:a9f2 */
void blit_or_own(FarPtr spr);                                           /* 06c9:aa12 */
void blit_and_hot(FarPtr spr, s16 x, s16 y);                            /* 06c9:808a AND unclipped */
void blit_and_raw(FarPtr spr, s16 x, s16 y);                            /* 06c9:80aa */
void blit_and_own(FarPtr spr);                                          /* 06c9:80ca */
void blit_xor_hot(FarPtr spr, s16 x, s16 y);                            /* 06c9:c132 XOR unclipped */
void blit_xor_raw(FarPtr spr, s16 x, s16 y);                            /* 06c9:c152 */
void blit_xor_own(FarPtr spr);                                          /* 06c9:c172 */
