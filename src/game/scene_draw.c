/* scene_render: background drawing — sky, mountains, clouds, ground scanlines, tunnel walls, tunnel
 * ribs for the front view (06c9:0919..0d12, 1ad6) and the mirror (06c9:26d8..2957, 34dc).
 * port/spec/scene_render.md §4.6, §4.8.1. */
#include "scene.h"

static void fill(s16 x, s16 y, s16 w, s16 h, u16 colour)       /* 06c9:89a2 */
{
    gfx_fill_rect_clip(x, y, w, h, (u8)colour);
}

/* 06c9:0919 draw_sky_front / 06c9:26d8 draw_sky_mirror — §4.6 */
void draw_sky(const SceneView *v)
{
    s16 ax = VS(v, top_sy);
    s16 wd = v->width;
    u16 sky = DSW(DS_col_sky);
    if (DSB(DS_toggle_37f7) == 0) {
        u8 bl = VB(v, r0_any);
        if (bl & 0x80) {                                  /* tunnel in view */
            s16 di = VS(v, tunnel_out_l), bp = VS(v, tunnel_out_r);
            if (VB(v, r0_state) & 0x80) {
                fill(di, VS(v, tunnel_in_top), (s16)(bp - di),
                     (s16)(VS(v, tunnel_out_top) - VS(v, tunnel_in_top)), 0);
                return;
            }
            s16 top = VS(v, tunnel_in_top), ceil = VS(v, tunnel_ceiling);
            if (bl & 0x40) {
                s16 lc = VS(v, left_cut_x);
                fill(lc, ceil, (s16)(bp - lc), (s16)(VS(v, top_sy) - ceil), sky);
            } else {
                fill(di, ceil, (s16)(VS(v, right_cut_x) - di), (s16)(VS(v, top_sy) - ceil), sky);
            }
            fill(di, top, (s16)(bp - di), (s16)(ceil - top), 0);
            return;
        }
        if (bl & 0x40) {
            s16 lc = VS(v, left_cut_x);
            if (bl & 0x08) fill(lc, 0, (s16)(VS(v, right_cut_x) - lc), ax, sky);
            else           fill(lc, 0, (s16)(wd - lc), ax, sky);
            return;
        }
        if (bl & 0x08) {
            fill(0, 0, VS(v, right_cut_x), ax, sky);
            return;
        }
    }
    fill(0, 0, wd, ax, sky);
    if (DSB(DS_backdrop_off) != 0) return;
    u16 hb = v->sky_handles;
    if (DSW((u16)(hb + 8 * 4 + 2)) == 0) return;          /* mtn0 / rmt0 segment */
    s16 heading = (s16)(s8)(u8)((u16)(DSW(DS_view_yaw) << 3) >> 8);
    if (v->front) {
        s16 di = (s16)((u16)-(u16)(DSW(DS_heading) - (u16)heading) & 0x3FF);  /* DS:5344 mountain scroll */
        s16 y = VS(v, top_sy);
        blit_copy_clip_hot(hnd((u16)(hb + 8 * 4)),  (s16)(di - 1024), y);
        blit_copy_clip_hot(hnd((u16)(hb + 10 * 4)), (s16)(di - 500), y);
        blit_copy_clip_hot(hnd((u16)(hb + 9 * 4)),  (s16)(di - 300), y);
        blit_copy_clip_hot(hnd((u16)(hb + 8 * 4)),  di, y);
        if (DSW((u16)(hb + 11 * 4 + 2)) == 0) return;     /* clo1 segment */
        di = (s16)((u16)-(u16)(DSW(DS_cloud_scroll) - (u16)heading) & 0x3FF);
        y = (s16)(RW(v, row_sy, 0x76) - 30);
        di = (s16)(di - 100);
        blit_copy_clip_hot(hnd((u16)(hb + 11 * 4)), (s16)(di - 1024), y);
        blit_copy_clip_hot(hnd((u16)(hb + 13 * 4)), (s16)(di - 500), y);
        blit_copy_clip_hot(hnd((u16)(hb + 12 * 4)), (s16)(di - 300), y);
        blit_copy_clip_hot(hnd((u16)(hb + 11 * 4)), di, y);
    } else {
        s16 di = (s16)((u16)(DSW(DS_heading) - (u16)heading) & 0x3FF);
        di = (s16)(di >> 1);
        s16 y = RW(v, row_sy, 0x30);
        blit_copy_clip_hot(hnd((u16)(hb + 8 * 4)),  (s16)(di - 512), y);   /* rmt0 */
        blit_copy_clip_hot(hnd((u16)(hb + 10 * 4)), (s16)(di - 250), y);   /* rmt2 */
        blit_copy_clip_hot(hnd((u16)(hb + 9 * 4)),  (s16)(di - 150), y);   /* rmt1 */
        blit_copy_clip_hot(hnd((u16)(hb + 8 * 4)),  di, y);
    }
}

/* 06c9:0c17 span_fill_to: fills the current scanline (row offset *di in the cached plane segments
 * DS:5420..5426) from span_x to end - 1, the last byte to its end. Returns true when the original jumps
 * to [DS:0702] (scanline full), i.e. the caller continues with the next scanline. */
static bool span_fill_to(u16 *di, s16 end, u16 colour)
{
    s16 dx = DSS(DS_span_x);
    if (end <= dx) return false;
    dx = (s16)(dx >> 3);
    DSS(DS_span_x) = end;
    u16 c = colour & 0xF;
    u8 p[4];
    for (int k = 0; k < 4; k++) {
        p[k] = DSB((u16)(DS_colour_plane_tables + 0x10 * k + c));
        DSB((u16)(0x08AE + k)) = p[k];                    /* scratch bytes DS:08AE..08B1 */
    }
    s16 e = (s16)(end - 1);
    u16 ebit = (u16)(e & 7);
    s16 eb = (s16)(e >> 3);
    u8 cnt = (u8)(eb - dx);                                /* xor ch,ch */
    u8 m = DSB((u16)(DS_mask_from_bit + DSW(DS_span_bit)));
    for (int k = 0; k < 4; k++) {
        u16 seg = DSW((u16)(DS_plane_seg + 2 * k));
        u8 *b = mp(seg, *di);
        *b = (u8)((*b & (u8)~m) | (p[k] & m));
    }
    if (cnt != 0) {
        *di = (u16)(*di + 1);
        for (int k = 0; k < 4; k++) {
            u16 seg = DSW((u16)(DS_plane_seg + 2 * k));
            for (u16 n = 0; n < cnt; n++) wr8(seg, (u16)(*di + n), p[k]);
        }
        *di = (u16)(*di + cnt - 1);
    }
    u16 sb = (u16)(ebit + 1);
    DSW(DS_span_bit) = sb;
    if (sb == 8) {
        DSW(DS_span_bit) = 0;
        *di = (u16)(*di + 1);
        if (DSW(DS_span_x) == DSW(DS_view_width)) return true;   /* jmp [DS:0702] */
    }
    return false;
}

/* 06c9:0b0d draw_ground_front / 06c9:284f draw_ground_mirror — §4.6 */
void draw_ground(const SceneView *v)
{
    u16 wd = (u16)v->width;
    u16 end2 = (u16)(v->height * 2);
    u16 si = (u16)(VW(v, top_sy) << 1);
    u16 rowtab = DSW(v->rowtab);
    /* PORT: the loop is a do-while ending at si == height * 2; top_sy >= height would run it ~32000
     * times over random rows. Never happens with the shipped data; treated as empty (as clamp_spans). */
    if ((s16)si >= (s16)end2) return;
    do {
        u16 di = CSW((u16)(si + rowtab));
        DSW(DS_span_bit) = 0;
        DSW(DS_span_x) = 0;
        u16 f = DSW((u16)(DS_span_flags + si));
        u16 bx;
        if (f & 0x20) {                                   /* left drop-off */
            bx = DSW(DS_col_sky);
            if ((s16)si >= VS(v, left_sky_sy2) && VS(v, left_sky_x) < SPAN(span_ol, si)) {
                if (span_fill_to(&di, VS(v, left_sky_x), bx)) goto next;
                bx = DSW(DS_scene_words);                 /* DS:534A col_left */
            }
        } else {
            bx = DSW(DS_scene_words);
        }
        if (span_fill_to(&di, SPAN(span_ol, si), bx)) goto next;
        if (span_fill_to(&di, SPAN(span_l, si), DSW(DS_col_shoulder))) goto next;
        if (span_fill_to(&di, SPAN(span_r, si), 7)) goto next;
        if (span_fill_to(&di, SPAN(span_or, si), DSW(DS_col_shoulder))) goto next;
        if (DSW((u16)(DS_span_flags + si)) & 0x04) {       /* right drop-off */
            if ((s16)si >= VS(v, right_sky_sy2) && VS(v, right_sky_x) >= SPAN(span_or, si)) {
                if (span_fill_to(&di, VS(v, right_sky_x), DSW(DS_col_right))) goto next;
            }
            if (span_fill_to(&di, (s16)wd, DSW(DS_col_sky))) goto next;
        } else {
            if (span_fill_to(&di, SPAN(span_band, si), DSW(DS_col_right))) goto next;
            if (span_fill_to(&di, (s16)wd, DSW(DS_col_far))) goto next;
        }
    next:
        si = (u16)(si + 2);
    } while (si != end2);
}

/* 06c9:0d01 hline: DX = x0, AX = x1, DI = y, BX = colour */
void hline(s16 dx, s16 ax, s16 di, u16 bx)
{
    if (dx < ax) gfx_draw_line(dx, di, ax, di, (u8)bx);
}

/* 06c9:0bb8 draw_tunnel_walls_front / 06c9:28f9 draw_tunnel_walls_mirror — §4.6 */
void draw_tunnel_walls(const SceneView *v)
{
    /* the original doubles tunnel_in_sy for the loop and halves it again afterwards */
    s16 end2 = (s16)(VW(v, tunnel_in_sy) << 1);
    s16 di = VS(v, tunnel_out_sy);
    s16 si = (s16)(di << 1);
    if (si >= end2) return;
    do {
        s16 l = SPAN(span_l, (u16)si);
        u16 bx;
        if (DSB(DS_toggle_37f7) == 0) {
            hline(VS(v, tunnel_in_l), l, di, 0);
            hline(SPAN(span_l, (u16)si), SPAN(span_r, (u16)si), di, 8);
            bx = 0;
        } else {
            hline(VS(v, tunnel_in_l), l, di, 8);
            bx = 8;
        }
        hline(SPAN(span_r, (u16)si), VS(v, tunnel_in_r), di, bx);
        di++;
        si = (s16)(si + 2);
    } while (si < end2);
}

/* 06c9:1ad6 tunnel_rib_front / 06c9:34dc tunnel_rib_mirror: white frame at row SI */
void tunnel_rib(const SceneView *v, u16 si)
{
    s16 bx = RW(v, row_sy, si);
    s16 cx = (s16)(bx - (s16)(RU(v, w, si) >> 1));
    if (DSB(DS_toggle_37f7) != 0) cx = (s16)(VS(v, top_sy) - 5);
    s16 ax = RW(v, row_l, si), di = RW(v, row_r, si);
    if (DSB(DS_toggle_37f7) == 0) gfx_draw_line(ax, cx, di, cx, 0xFF);
    gfx_draw_line(di, cx, di, bx, 0xFF);
    gfx_draw_line(ax, cx, ax, bx, 0xFF);
}
