/* scene_render: per-row objects, drawn far to near — road markings, cliffs and their decorations,
 * tunnel mouths and lights, road objects (signs, cross bands, gas station / FINISH, hazards), scenery
 * sprites and SGN text signs, poles, traffic, opponent and police.
 * Front view 06c9:0d13..1ad5, mirror 06c9:2958..34db. port/spec/scene_render.md §4.8-§4.11. */
#include "scene.h"

static void fill(s16 x, s16 y, s16 w, s16 h, u16 colour)       /* 06c9:89a2 */
{
    gfx_fill_rect_clip(x, y, w, h, (u8)colour);
}

static void and_ch(u16 h, s16 x, s16 y) { blit_and_clip_hot(hnd(h), x, y); }
static void or_ch(u16 h, s16 x, s16 y)  { blit_or_clip_hot(hnd(h), x, y); }
static void xor_ch(u16 h, s16 x, s16 y) { blit_xor_clip_hot(hnd(h), x, y); }

/* inline at 06c9:0d22 / 13bf (mirror 2967 / 2f1d) */
static void set_ceiling_clip(const SceneView *v, u16 si)
{
    if (DSB(DS_toggle_37f7) == 0) CSW(GFX_CUR_CLIP_Y0) = VW(v, tunnel_ceiling);
    if ((VB(v, r0_any) & 0x80) && (s16)si <= VS(v, tunnel_ceiling_row)) CSW(GFX_CUR_CLIP_Y0) = 0;
}

/* road markings of a visible row: yellow centre dot, white lane lines (0d97 / 29dc) */
static void markings(const SceneView *v, u16 si, s16 y, u8 fl)
{
    u16 wd = (u16)v->width;
    u16 di = CSW((u16)(DSW(v->rowtab) + (u16)(y << 1)));
    s16 x = RW(v, row_cx, si);
    if ((u16)x < wd) {
        u16 bx = (u16)(di + (u16)(x >> 3));
        u8 m = DSB((u16)(DS_pixel_mask + (x & 7)));
        mp(DSW(DS_plane_seg), bx)[0] &= (u8)~m;
        mp(DSW(DS_plane_seg + 2), bx)[0] |= m;
        mp(DSW(DS_plane_seg + 4), bx)[0] |= m;
        mp(DSW(DS_plane_seg + 6), bx)[0] |= m;
    }
    if (!(fl & 1) || (VB(v, dash_phase) & 4)) return;
    for (int side = 0; side < 2; side++) {
        if (side == 1 && DSB(DS_median) == 0) break;
        u16 W = RU(v, w, si);
        x = side == 0 ? (s16)(RW(v, row_cx, si) + (s16)W) : (s16)(RW(v, row_cx, si) - (s16)W);
        if ((u16)x >= wd) continue;
        u16 bx = (u16)(di + (u16)(x >> 3));
        u8 m = DSB((u16)(DS_pixel_mask + (x & 7)));
        for (int k = 0; k < 4; k++) mp(DSW((u16)(DS_plane_seg + 2 * k)), bx)[0] |= m;
    }
}

/* cliff wall at its cut row (0e7e / 0f71; mirror 2ac3 / 2b43) */
static void cliff_wall(const SceneView *v, u16 si, bool left)
{
    s16 ax = left ? VS(v, left_cut_x) : VS(v, right_cut_x);
    s16 cx = RW(v, row_clip, si);
    s16 di, bp;
    if ((VB(v, r0_any) & 0x80) && (s16)si >= VS(v, tunnel_out_row)) {
        di = VS(v, tunnel_ceiling);
        cx = (s16)(cx - di);
        bp = left ? VS(v, tunnel_out_l) : VS(v, tunnel_out_r);
    } else {
        di = 0;
        bp = left ? 0 : v->width;
    }
    if (left) fill(bp, di, (s16)(ax - bp), cx, 6);
    else      fill(ax, di, (s16)(bp - ax), cx, 6);
    s16 x = left ? RW(v, row_ol, si) : RW(v, row_or, si);
    s16 dy = RW(v, row_sy, si);
    if (dy >= v->height) dy = v->height;
    u16 hb = v->sky_handles;
    u16 k = left ? 4 : 0;                                 /* lcfA/lcfa (lcfC/lcfc), rcfA/rcfa (rcfC/rcfc) */
    and_ch((u16)(hb + 4 * k), x, dy);
    or_ch((u16)(hb + 4 * (k + 1)), x, dy);
}

/* cliff decoration sprites (front only; 0ef9 / 0fed) */
static void cliff_deco(const SceneView *v, u16 si, bool left)
{
    u16 bx = (u16)((u8)(VB(v, dash_phase) << 1) & 0x1E);
    u16 w = DSW((u16)(DS_cliff_deco_pattern + bx));
    if ((u8)w == 0) return;
    s16 x = left ? RW(v, row_ol, si) : RW(v, row_or, si);
    if ((u16)x >= 320) return;
    u8 cl = (u8)(w >> 8);
    u16 ax = (u16)((u16)((u8)w - 1) << 4);
    ax = (u16)(ax + (left ? SCN_H(184) : SCN_H(160)));    /* 0x2092 rcka/wedA/lin1, 0x2032 rcka/wed0/linA */
    u16 di = (u16)(VW(v, scale4) + ax);
    s16 dx = RW(v, row_sy, si);
    if (cl != 0) dx = (s16)(dx - (u16)((u8)(VB(v, obj_size) >> 1) * cl));
    and_ch((u16)(di + 0x30), x, dx);                      /* upper case = mask */
    or_ch(di, x, dx);
}

/* §4.8.1 tunnel mouths (105f..13bc; mirror 2bbe..2f1a) */
static void tunnel_mouths(const SceneView *v, u16 si)
{
    bool style = DSB(DS_toggle_37f7) != 0;
    s16 wd = v->width;
    u16 hb = v->sky_handles;
    u16 sky = DSW(DS_col_sky);
    if (si == VW(v, tunnel_out_row)) {                    /* far end */
        u16 dx = 0;
        s16 bx = VS(v, tunnel_in_top), cx = VS(v, tunnel_out_sy);
        s16 ax = VS(v, tunnel_in_l), di = VS(v, tunnel_out_l);
        if (style) {
            dx = 8;
            di = RW(v, row_l, si);
            bx = (s16)(VS(v, top_sy) - 5);
        }
        if (di <= ax) { s16 t = di; di = ax; ax = t; }
        cx = (s16)(cx - bx);
        di = (s16)(di - ax);
        s16 ax2 = VS(v, tunnel_out_r), di2 = VS(v, tunnel_in_r);
        if (style) {
            ax2 = RW(v, row_r, si);
            if (di2 <= ax2) { s16 t = di2; di2 = ax2; ax2 = t; }
        }
        di2 = (s16)(di2 - ax2);
        fill(ax2, bx, di2, cx, dx);
        fill(ax, bx, di, cx, dx);
        if (style) tunnel_rib(v, si);
    } else if (!style && (VB(v, r0_state) & 0x80)) {
        if (si == VW(v, right_cut_row)) {
            s16 bx = VS(v, tunnel_in_top);
            s16 ax = VS(v, right_cut_x);
            fill(ax, bx, (s16)(VS(v, tunnel_in_r) - ax), (s16)(RW(v, row_clip, si) - bx), 0);
        }
        if (si == VW(v, left_cut_row)) {
            s16 bx = VS(v, tunnel_in_top);
            s16 dx = VS(v, tunnel_in_l);
            fill(dx, bx, (s16)(VS(v, left_cut_x) - dx), (s16)(RW(v, row_clip, si) - bx), 0);
        }
    }
    if (si != VW(v, tunnel_in_row)) return;               /* near end (entrance) */
    s16 clip = RW(v, row_clip, si);
    s16 in_l = VS(v, tunnel_in_l), in_r = VS(v, tunnel_in_r), in_top = VS(v, tunnel_in_top);
    if (DSB(DS_start_flags) & 0x80) {                     /* car already inside */
        if (!style) {
            fill(0, 0, in_l, clip, 0);
            fill(in_r, 0, (s16)(wd - in_r), clip, 0);
        } else {
            s16 t = (s16)(VS(v, top_sy) - 5), h = (s16)(clip - t);
            fill(in_r, t, (s16)(wd - in_r), h, 8);
            fill(0, t, in_l, h, 8);
        }
        return;
    }
    if (style) {
        tunnel_rib(v, si);
        return;
    }
    s16 top_sy = VS(v, top_sy);
    if (!(VB(v, r0_state) & 0x40)) {                      /* portal A */
        s16 bp = in_l, di = VS(v, left_sky_x), px;
        bool first;
        if ((s16)si > VS(v, left_sky_row)) first = true;
        else if (di == 0) first = false;
        else { di = (s16)(di + v->sky_cut); first = bp <= di; }
        if (first) {
            fill(0, 0, bp, top_sy, sky);
            px = RW(v, row_l, si);
        } else {
            fill(di, 0, (s16)(bp - di), clip, 6);
            bp = di;
            if (bp == 0) goto walls_a;
            fill(0, 0, bp, top_sy, sky);
            px = bp;
        }
        and_ch((u16)(hb + 2 * 4), px, v->portal_y);       /* rcfB / rcfD */
        or_ch((u16)(hb + 3 * 4), px, v->portal_y);        /* rcfb / rcfd */
    walls_a:
        fill(bp, 0, (s16)(wd - bp), in_top, 6);
        fill(in_r, in_top, (s16)(wd - in_r), (s16)(clip - in_top), 6);
    } else {                                              /* portal B (left cliff state) */
        s16 bp = in_r, di = VS(v, right_sky_x), px;
        bool first;
        if ((s16)si > VS(v, right_sky_row)) first = true;
        else if (di == wd) first = false;
        else { di = (s16)(di - v->sky_cut); first = bp >= di; }
        if (first) {
            fill(bp, 0, (s16)(wd - bp), top_sy, sky);
            px = RW(v, row_r, si);
        } else {
            fill(in_r, 0, (s16)(di - in_r), clip, 6);
            bp = di;
            fill(bp, 0, (s16)(wd - bp), top_sy, sky);
            px = bp;
        }
        and_ch((u16)(hb + 6 * 4), px, v->portal_y);       /* lcfB / lcfD */
        or_ch((u16)(hb + 7 * 4), px, v->portal_y);        /* lcfb / lcfd */
        fill(0, 0, bp, in_top, 6);
        fill(0, in_top, in_l, (s16)(clip - in_top), 6);
    }
    if (si != 0) tunnel_rib(v, si);
}

/* §4.9 road object r3 = o (1..20); jump tables DS:12DA / DS:2C36 */
static void road_object(const SceneView *v, u16 si, u8 o)
{
    s16 y = RW(v, row_sy, si);
    u16 W = RU(v, w, si);
    s16 R = RW(v, row_r, si), L = RW(v, row_l, si);
    if (o >= 1 && o <= 9) {                               /* signs on both sides */
        u16 di = (u16)(((u16)(o - 1) << 4) + ROAD_H(ROAD_sa) + VW(v, scale4));
        u16 bx = (u16)(VW(v, scale4) + ROAD_H(ROAD_pst0));
        s16 dy = (s16)(y - VS(v, obj_size));
        or_ch(bx, R, dy);
        or_ch(bx, L, dy);
        and_ch(di, R, dy);
        and_ch(di, L, dy);
        if (v->front) {                                   /* the mirror never draws the sign image */
            or_ch((u16)(di + 0x90), R, dy);
            or_ch((u16)(di + 0x90), L, dy);
        }
    } else if (o == 10 || o == 12) {                      /* white band across the road */
        u16 bx, end2;
        if (v->front) {
            u16 ax = (u16)y;
            if (ax >= 0x5B) ax = 0x5B;
            bx = ax;
            ax = si < 6 ? 0x5B : RU(v, row_sy, si - 6);
            end2 = (u16)(ax << 1);
            DSW(0x1304) = end2;
        } else {
            u16 ax = (u16)y;
            if (ax >= 0x10) ax = 0x10;
            bx = RU(v, row_sy, si + 6);
            end2 = (u16)(ax << 1);
            DSW(0x2C60) = end2;
            if (si > 0x2C) bx = VW(v, top_sy);
        }
        CSW(GFX_CUR_CLIP_Y1) = (u16)v->height;
        u16 s = (u16)(bx << 1);
        do {
            hline(SPAN(span_l, s), SPAN(span_r, s), (s16)(s >> 1), 15);
            s = (u16)(s + 2);
        } while ((s16)s <= (s16)end2);
        CSW(GFX_CUR_CLIP_Y1) = RU(v, row_clip, si);
    } else if (o == 11) {                                 /* end of stage */
        if (DSW(DS_last_stage) == 0) {                    /* gas station sign */
            u16 di = (u16)(VW(v, scale5) + ROAD_H(ROAD_GST0));
            s16 dx = (s16)(W + R);
            and_ch(di, dx, y);
            or_ch((u16)(di + 0x14), dx, y);
        } else {                                          /* FINISH banner */
            s16 ax = (s16)((s16)W >> 1);
            s16 dx = (s16)(ax >> 1);
            ax = (s16)(ax + dx);
            dx = (s16)(dx >> 1);
            s16 bp = (s16)(y - ax);
            s16 cx = R;
            s16 bx = (s16)(L - dx);
            s16 bxp = (s16)(bx + dx);
            fill(bxp, bp, (s16)(cx - bxp), (s16)(dx << 1), 0xFFFF);   /* banner */
            fill(cx, bp, dx, ax, 0xFFFF);                              /* right post */
            fill(bx, bp, dx, ax, 0xFFFF);                              /* left post */
            if (v->front) {                               /* the mirror draws no letters */
                u16 ox = (u16)(bxp + ((u16)((u16)dx << 1) >> 1));
                u16 s = RU(v, xs, si);
                DSW(0x1304) = s;
                for (u16 p = DS_finish_letters;;) {
                    u16 w0 = DSW(p); p = (u16)(p + 2);
                    if ((s16)w0 < 0) break;
                    u16 xa = (u16)((((u32)w0 * s) >> 16) + ox);
                    u16 w1 = DSW(p); p = (u16)(p + 2);
                    u16 ya = (u16)((((u32)w1 * s) >> 16) + (u16)bp);
                    u16 w2 = DSW(p); p = (u16)(p + 2);
                    u16 xb = (u16)((((u32)w2 * s) >> 16) + ox);
                    u16 w3 = DSW(p); p = (u16)(p + 2);
                    u16 yb = (u16)((((u32)w3 * s) >> 16) + (u16)bp);
                    gfx_draw_line((s16)xb, (s16)yb, (s16)xa, (s16)ya, 0);
                }
            }
        }
    } else if (o >= 13 && o <= 20) {                      /* hazards (XOR) */
        u8 al = (u8)(o - 10);
        u16 cx = (u16)(W >> 1);
        u16 ax = (u16)(al << 1);
        if (ax & 2) cx = (u16)-cx;                        /* odd objects: left half */
        cx = (u16)(cx + (u16)RW(v, row_cx, si));
        ax = (u8)(((u8)ax - 6) & 0xFC);
        u16 bx = (u16)((ax << 2) + VW(v, scale4));
        xor_ch((u16)(ROAD_H(ROAD_rck) + bx), (s16)cx, y);  /* rck / oil / gra / pot */
    }
}

/* §4.10 scenery sprites and SGN text signs (1628..1854; mirror 311c..327e) */
static void scenery(const SceneView *v, u16 si)
{
    u16 k = (u16)(VB(v, dash_phase) & 0x7F);
    s8 t = DSC((u16)(DS_dat_scenery_type + k));
    if (t < 0) return;
    u16 W = RU(v, w, si);
    if ((u8)t < 0x50) {
        u16 di = (u16)(((u16)(u8)t << 2) + VW(v, scale5) + DS_scenery_handles);
        if (DSW((u16)(di + 2)) != 0) {
            s16 off = (s16)DSC((u16)(DS_dat_scenery_offset + k));
            off = (s16)(off >= 0 ? off + 2 : off - 2);
            s16 x = (s16)((s16)(off * (s16)W) >> 3);
            x = (s16)(x + (x > 0 ? RW(v, row_r, si) : RW(v, row_l, si)));
            s16 y = RW(v, row_sy, si);
            and_ch(di, x, y);
            or_ch((u16)(di + 0x140), x, y);
            return;
        }
    }
    if ((u8)t < 0x1E) return;
    u16 sgn = DSW(DS_sgn_segment);
    if (sgn == 0) return;
    u16 q = (u16)((u8)((u8)t - 0x1E) / 5);
    u16 ofs = rd16(sgn, (u16)(q << 1));
    if (ofs == 0) return;
    for (u16 n = 0; n < 0x2D; n++)                        /* rep movsw from the SGN segment */
        DSW((u16)(DS_sign_rec + 2 * n)) = rd16(sgn, (u16)(ofs + 2 * n));
    DSW(0x52E4) = (u16)(mid16(DSW(0x52E4), W) >> 1);     /* width */
    DSW(0x52E6) = (u16)(mid16(DSW(0x52E6), W) >> 1);     /* height */
    DSW(0x52FC) = (u16)(mid16(DSW(0x52FC), W) >> 1);     /* post height */
    u16 pw = (u16)(W >> 4);
    DSW(0x52E0) = pw;
    s16 off = (s16)DSC((u16)(DS_dat_scenery_offset + k));
    u16 half = (u16)(DSW(0x52E4) >> 1);
    s16 x = (s16)((s16)(off * (s16)W) >> 3);
    if (x > 0) x = (s16)(x + RW(v, row_r, si) - (s16)half);
    else       x = (s16)(x + RW(v, row_l, si) - (s16)half);
    DSS(0x52E2) = x;                                      /* sign x */
    s16 post_h = DSS(0x52FC);
    s16 cy = (s16)(RW(v, row_sy, si) - post_h);
    u16 post_c = DSW(0x52F4);
    s16 sw = DSS(0x52E4), sh = DSS(0x52E6);
    s16 board_top = (s16)(cy - sh);
    DSS(0x52E6) = board_top;                              /* sign y */
    fill(x, board_top, sw, sh, DSW(0x52F0));             /* board */
    fill((s16)(x + sw - (s16)pw), cy, (s16)pw, post_h, post_c);
    fill(x, cy, (s16)pw, post_h, post_c);

    /* text, vector font DS:8430 */
    DSW(0x52E0) = DSW(0x52F8);                            /* x0 = margin x */
    u16 p = 0x52FE;
    for (;;) {
        s8 c = DSC(p); p = (u16)(p + 1);
        if (c < 10) break;
        if (c == 10) {
            DSW(0x52F8) = DSW(0x52E0);
            DSW(0x52FA) = (u16)(DSW(0x52FA) + DSW(0x52EA));
            continue;
        }
        u16 g = rd16(DSW(DS_fnt_segment), (u16)(((u16)(u8)c - 0x20) << 1));
        if (g != 0) {
            for (;;) {
                u16 fnt = DSW(DS_fnt_segment);
                u8 b = rd8(fnt, g);
                if (b == 0xFF) break;
                g++;
                DSW(0x52FC) = (u16)(mid16((u16)(b + DSW(0x52F8)), W) >> 1);
                b = rd8(fnt, g++);
                u16 bp = (u16)(mid16((u16)(b + DSW(0x52FA)), W) >> 1);
                b = rd8(fnt, g++);
                u16 cx = (u16)(mid16((u16)(b + DSW(0x52F8)), W) >> 1);
                b = rd8(fnt, g++);
                u16 ax = (u16)(mid16((u16)(b + DSW(0x52FA)), W) >> 1);
                u16 sx = DSW(0x52E2);
                cx = (u16)(cx + sx);
                DSW(0x52FC) = (u16)(DSW(0x52FC) + sx);
                bp = (u16)(bp + DSW(0x52E6));
                ax = (u16)(ax + DSW(0x52E6));
                gfx_draw_line((s16)DSW(0x52FC), (s16)bp, (s16)cx, (s16)ax, (u8)DSW(0x52EC));
            }
        }
        DSW(0x52F8) = (u16)(DSW(0x52F8) + DSW(0x52E8));
    }
}

/* poles every 16 units / tunnel rib (1855; mirror 327f) */
static void poles(const SceneView *v, u16 si)
{
    if (VB(v, dash_phase) & 0x0F) return;
    if (VB(v, r0_state) & 0x80) {
        tunnel_rib(v, si);
        return;
    }
    s16 cx = (s16)(RW(v, row_sy, si) - VS(v, obj_size));
    u16 bx = (u16)(VW(v, scale4) + ROAD_H(ROAD_pal0));
    u16 q = (u16)(RU(v, w, si) >> 2);
    s16 bp = (s16)(RW(v, row_l, si) - (s16)q);
    s16 dx = (s16)(RW(v, row_r, si) - 1 + (s16)q);
    u16 wd = (u16)v->width;
    if ((u16)dx < wd) { and_ch(bx, dx, cx); or_ch((u16)(bx + 0x10), dx, cx); }
    if ((u16)bp < wd) { and_ch(bx, bp, cx); or_ch((u16)(bx + 0x10), bp, cx); }
}

/* x of a car on row si: mulhi(lateral, xs) (sar 1 in the mirror) + centre */
static s16 car_x(const SceneView *v, u16 si, u16 lat)
{
    u16 dx = mulhi_s(lat, RU(v, xs, si));
    if (!v->front) dx = (u16)((s16)dx >> 1);
    return (s16)(dx + RU(v, row_cx, si));
}

static void cars(const SceneView *v, u16 si)
{
    bool front = v->front;
    s16 y = (s16)(RW(v, row_sy, si) - 1);
    u16 scale4c = (u16)(RU(v, carscale, si) << 2);

    /* traffic, list order reversed (18d8 / 3300) */
    for (s32 di = (s16)DSW(v->draw_list_len);;) {
        di -= 8;
        if (di < 0) break;
        u16 e = (u16)(v->draw_list + di);
        if (DSW(e) != si) continue;
        s16 x = car_x(v, si, DSW((u16)(e + 4)));
        u16 bx = (u16)(DSW((u16)(e + 2)) - 1);
        if (!front) bx ^= 4;                              /* rear views in the mirror */
        u16 cx = (u16)((bx & 4) << 1);
        bx = (u16)(((bx & 3) << 4) + cx);
        bx = (u16)(((bx << 1) + RU(v, carscale, si)) << 2);
        and_ch((u16)(DS_traffic1_handles + bx), x, y);
        or_ch((u16)(DS_traffic1_handles + 0x20 + bx), x, y);
    }

    /* opponent (194f / 337c) */
    if (DSB(DS_opponent_enabled) != 0 && si == DSW(v->opp_row2)) {
        s16 x = car_x(v, si, DSW(v->opp_lat));
        u16 bx = scale4c;
        if (front) {
            bool brake = DSB(DS_r_opp_brake) != 0;
            FarPtr brk = hnd((u16)(DS_opp_road_handles + 0x80 + bx));   /* brk0-7, pushed first */
            if (DSB(v->opp_alt) != 0) bx = (u16)(bx + 0x40);
            and_ch((u16)(DS_opp_road_handles + 0x20 + bx), x, y);       /* rc?M / ra?M */
            or_ch((u16)(DS_opp_road_handles + bx), x, y);               /* rcr? / rac? */
            if (brake) blit_xor_clip_hot(brk, x, y);
        } else {
            if (DSB(v->opp_alt) != 0) bx = (u16)(bx + 0x40);
            and_ch((u16)(DS_opp_front_handles + 0x20 + bx), x, y);      /* fc?M / fa?M */
            or_ch((u16)(DS_opp_front_handles + bx), x, y);              /* fcr? / fac? */
        }
    }

    /* police (19d0 / 33df) */
    if (VB(v, r_cop_active) != 0 && si == DSW(v->cop_row2)) {
        s16 x = car_x(v, si, DSW(v->cop_lat));
        u16 bx = scale4c;
        if (front) {
            bool brake = DSB(DS_r_cop_brake) != 0;
            FarPtr brk = hnd((u16)(DS_cop_extra_handles + 0x20 + bx));  /* brk0-7 */
            and_ch((u16)(DS_cop_car_handles + 0x40 + bx), x, y);        /* COP rc?M */
            or_ch((u16)(DS_cop_car_handles + 0x60 + bx), x, y);         /* COP rcr? */
            if (brake) blit_xor_clip_hot(brk, x, y);
        } else {
            and_ch((u16)(DS_cop_car_handles + bx), x, y);               /* COP fc?M */
            or_ch((u16)(DS_cop_car_handles + 0x20 + bx), x, y);         /* COP fcr? */
        }
        if (DSW(DS_stage_time) & 1) xor_ch((u16)(DS_cop_extra_handles + bx), x, y);   /* CLR light bar */
    }

    /* parked police car, one row nearer (1a6e / 345f) */
    if (VB(v, r_cop_state) >= 7 && (u16)(si + 2) == DSW(v->cop_row2)) {
        u16 di = (u16)((RU(v, carscale, si) << 4) + DS_cop_extra_handles + 0x40
                       + (DSW(DS_sim_tick10) & 0x0C));     /* DS:3346 radar flags */
        s16 xr = RW(v, row_r, si);
        and_ch((u16)(di + 0x80), xr, y);                   /* cp.. */
        or_ch(di, xr, y);                                  /* CP.. */
    }
}

static void draw_objects(const SceneView *v)
{
    bool front = v->front;
    u16 si = (u16)(v->nrows2 - 2);
    VB(v, dash_phase) = front ? (u8)(VB(v, unit_phase) + 0x3B) : (u8)(VB(v, unit_phase) - 0x18);
    u8 saved = VB(v, r0_state);                           /* state after the farthest row */
    for (;;) {
        set_ceiling_clip(v, si);
        u16 W = RU(v, w, si);
        u16 os = (u16)(W >> 3);
        if (os >= 0x1F) os = 0x1F;
        VW(v, obj_size) = os;
        u8 al = (u8)os;
        if ((s8)al >= 0x10) al = 0x10;
        VW(v, scale5) = (u16)(al & 0xFC);
        VW(v, scale4) = (u16)((os >> 1) & 0xFC);
        CSW(GFX_CUR_CLIP_Y1) = RU(v, row_clip, si);

        /* 1. road markings */
        s16 y = RW(v, row_sy, si);
        u8 fl = RB(v, row_flags, si);
        if (!(y > RW(v, row_clip, si) || y == v->height) && ((fl & 1) || !(VB(v, dash_phase) & 4)))
            markings(v, si, y, fl);

        /* 2. left cliff, 3. right cliff */
        if (VB(v, r0_state) & 0x40) {
            if (si == VW(v, left_cut_row) && !(VB(v, r0_state) & 0x80)) cliff_wall(v, si, true);
            if (front && !(VB(v, r0_state) & 0x80) && (s16)si < 0x2E && (s16)si < VS(v, left_cut_row))
                cliff_deco(v, si, true);
        }
        if (VB(v, r0_state) & 0x08) {
            if (si == VW(v, right_cut_row) && !(VB(v, r0_state) & 0x80)) cliff_wall(v, si, false);
            if (front && !(VB(v, r0_state) & 0x80) && (s16)si < 0x2E && (s16)si < VS(v, right_cut_row))
                cliff_deco(v, si, false);
        }
        CSW(GFX_CUR_CLIP_Y0) = 0;

        /* 4. tunnels */
        if (VB(v, r0_any) & 0x80) tunnel_mouths(v, si);
        set_ceiling_clip(v, si);

        /* 5. tunnel lights every 16 units */
        if (DSB(DS_toggle_37f7) == 0 && ((u8)(VB(v, dash_phase) + 8) & 0x0F) == 0
            && (VB(v, r0_state) & 0x80)) {
            u16 bx = (u16)(VW(v, scale5) + SCN_H(80));    /* SM00-04 */
            s32 cy = (s32)RW(v, row_sy, si) - (s16)(RU(v, w, si) >> 1);
            if (cy >= 0) or_ch(bx, RW(v, row_cx, si), (s16)cy);
        }

        /* 6. road object */
        {
            u8 o = RB(v, row_flags, si + 1);
            if (o != 0 && o < 0x15) road_object(v, si, o);
        }

        /* 7. scenery (front: rows < 44) */
        if (!front || (s16)si < 0x58) scenery(v, si);

        /* 8. poles, 9-12. cars */
        poles(v, si);
        cars(v, si);

        if (front) VB(v, dash_phase)--;
        else VB(v, dash_phase)++;
        VB(v, r0_state) ^= RB(v, row_flags, si);
        si = (u16)(si - 2);
        if ((s16)si < 0) break;
    }
    VB(v, r0_state) = saved;
}

/* 06c9:0d13 draw_front_objects — §4.8 */
void draw_front_objects(void)
{
    draw_objects(&scene_front_view);
}

/* 06c9:2958 draw_mirror_objects — §4.11; ends with the mirror composite into the main view */
void draw_mirror_objects(void)
{
    draw_objects(&scene_mirror_view);
    select_main_view();
    blit_copy_own(ds_far(DS_mirror_sprite));
}
