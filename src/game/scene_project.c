/* scene_render: road projection — row projection, scanline spans, cut lines and span clamping for the
 * front view (06c9:0338..0918) and the mirror (06c9:21c3..26d7). port/spec/scene_render.md §4.4, §4.5.
 * Both views share one implementation, parameterised by a SceneView (scene.h). */
#include "scene.h"
#include "../platform/res.h"

const SceneView scene_front_view = {
    .front = true, .sc = 0, .nrows2 = 0x78, .width = 320, .height = 92, .horizon = 0x33,
    .centre = 0x7D, .band_default = 0x0A00, .cut_offset = 22, .out_row_default = 0x76, .sky_cut = 15,
    .portal_y = 0x5B, .skip_target = 0x0B7B,
    .row_ol = DS_row_ol, .row_l = DS_row_l, .row_cx = DS_row_cx, .row_r = DS_row_r, .row_or = DS_row_or,
    .row_sy = DS_row_sy, .row_clip = DS_row_clip, .row_flags = DS_row_flags, .row_state = DS_row_state,
    .row_band = DS_row_band,
    .xs = DS_xs_front, .ys = DS_ys_front, .w = DS_w_front, .carscale = DS_carscale_front,
    .desc = DS_drive_buf_desc, .sprite = DS_main_sprite, .rowtab = DS_main_rowtab,
    .sky_handles = DS_scenery_sky_handles,
    .draw_list = DS_front_draw_list, .draw_list_len = DS_front_draw_list_len,
    .opp_row2 = DS_opp_row2, .opp_lat = DS_opp_lat, .cop_row2 = DS_cop_row2, .cop_lat = DS_cop_lat,
    .opp_alt = DS_r_opp_alt,
};

const SceneView scene_mirror_view = {
    .front = false, .sc = 0x195C, .nrows2 = 0x32, .width = 80, .height = 17, .horizon = 8,
    .centre = 0x28, .band_default = 0x0280, .cut_offset = 6, .out_row_default = 0x30, .sky_cut = 3,
    .portal_y = 0x10, .skip_target = 0x28BD,
    .row_ol = DS_row_ol_m, .row_l = DS_row_l_m, .row_cx = DS_row_cx_m, .row_r = DS_row_r_m,
    .row_or = DS_row_or_m, .row_sy = DS_row_sy_m, .row_clip = DS_row_clip_m, .row_flags = DS_row_flags_m,
    .row_state = DS_row_state_m, .row_band = DS_row_band_m,
    .xs = DS_xs_mirror, .ys = DS_ys_mirror, .w = DS_w_mirror, .carscale = DS_carscale_mirror,
    .desc = DS_mirror_desc, .sprite = DS_mirror_sprite, .rowtab = DS_mirror_rowtab,
    .sky_handles = DS_mirror_scenery_handles,
    .draw_list = DS_mirror_draw_list, .draw_list_len = DS_mirror_draw_list_len,
    .opp_row2 = 0x28C0, .opp_lat = 0x28C2, .cop_row2 = 0x28C4, .cop_lat = 0x28C6,
    .opp_alt = DS_r_opp_alt_m,
};

/* Lane widening of an edge (06c9:0438 / 0x04c6; mirror 06c9:22b7 / 0x2345): d = distance of the byte
 * just read from the last wide-road toggle. Returns the extra width (>= 0) and whether it applies. */
static bool widen(const SceneView *v, u16 W, u16 *ext)
{
    u16 dx = v->front ? (u16)(VW(v, walk_ptr) - 1) : (u16)(VW(v, walk_ptr) + 1);
    dx = (u16)(dx - DSW(DS_wide_toggle_ptr));          /* the mirror uses the front toggle pointer */
    u8 prev = VB(v, prev_road_byte);
    if (dx < 8) {
        if (!(prev & 0x80)) dx = (u16)(8 - dx);
        *ext = (u16)((u16)(W * dx) >> 3);
        return true;
    }
    if (prev & 0x80) { *ext = W; return true; }
    return false;
}

/* 06c9:0338 project_front_rows / 06c9:21c3 project_mirror_rows — §4.4 */
void project_rows(const SceneView *v)
{
    for (u16 si = 0; si != v->nrows2; si += 2) {
        u16 i = si;                                     /* word tables are indexed with SI */
        u16 bx = VW(v, walk_ptr);
        u8 al = DSB(bx);                                /* road byte (DGROUP address) */
        RB(v, row_flags, si) = (u8)(al >> 7);
        u8 cl = VB(v, prev_road_byte);
        VB(v, prev_road_byte) = al;
        if (v->front) {
            if (si != 0 && (s8)(u8)(cl ^ al) < 0) DSW(DS_wide_toggle_ptr) = bx;
            bx++;
        } else {
            bx--;                                       /* mirror: walks backwards, no toggle update */
        }
        VW(v, walk_ptr) = bx;
        u16 rec = (u16)((al & 0x7F) << 2);
        u16 w0 = DSW((u16)(DS_road_records + rec));     /* r0 | r1 << 8 */
        VW(v, rec_w0) = w0;
        VB(v, r0_state) ^= (u8)w0;
        VB(v, r0_any) |= (u8)w0;
        RB(v, row_flags, si) |= (u8)w0;
        RB(v, row_state, si) = VB(v, r0_state);
        u16 w1 = DSW((u16)(DS_road_records + 2 + rec)); /* r2 | r3 << 8 */
        VW(v, rec_w1) = w1;
        RB(v, row_flags, si + 1) = (u8)(w1 >> 8);

        /* height */
        s16 ax = (s16)(s8)(u8)w1;
        ax = (s16)-ax;
        ax = (s16)(ax >> 1);
        ax = (s16)(ax + VS(v, pitch_acc));
        VS(v, pitch_acc) = ax;
        s16 t = tan_deg8((u8)((u16)ax >> 8));
        s32 sum = (s32)t + VS(v, height_acc);
        VW(v, height_acc) = (u16)sum;
        s16 y = (s16)(mulhi_ge(sum >= 0, (u16)sum, RU(v, ys, i)) + v->horizon);
        RW(v, row_sy, si) = y;
        s16 top = VS(v, top_sy);
        if (y < top) {
            VS(v, top_sy) = y;
            top = y;
            VW(v, top_row) = si;
        }
        RW(v, row_clip, si) = top;

        /* heading and centre */
        s16 h = (s16)((s16)(s8)(u8)(VW(v, rec_w0) >> 8) * 16);
        h = (s16)(h + VS(v, heading_acc));
        if (h < -0x4600) h = -0x4600;
        else if (h > 0x4600) h = 0x4600;
        VS(v, heading_acc) = h;
        t = tan_deg8((u8)((u16)h >> 8));
        sum = (s32)t + VS(v, lat_acc);
        VW(v, lat_acc) = (u16)sum;
        s16 cx = (s16)(mulhi_ge(sum >= 0, (u16)sum, RU(v, xs, i)) + v->centre);
        RW(v, row_cx, si) = cx;
        u16 W = RU(v, w, i);
        s16 bp = (s16)(W >> 2);

        /* left edge (widening only with DAT+0x38C) */
        s16 L = (s16)(cx - (s16)W);
        u16 ext;
        if (DSB(DS_median) != 0 && widen(v, W, &ext)) L = (s16)(L - (s16)ext);
        RW(v, row_l, si) = L;
        s16 a = (s16)(L - bp);
        RW(v, row_ol, si) = a;
        u8 st = VB(v, r0_state);
        bool hit = false;
        if (st & 0xC0) {
            if (st & 0x80) a = (s16)(a + bp);           /* row_l */
            if (a > VS(v, left_cut_x)) {
                VS(v, left_cut_x) = a;
                VW(v, left_cut_row) = si;
                VB(v, left_cut_state) = st;
                hit = true;
            }
        }
        if (!hit) {
            a = RW(v, row_ol, si);
            if (si != 0 && a < RW(v, row_ol, si - 2) && RW(v, row_sy, si) <= VS(v, top_sy)
                && a < VS(v, left_sky_x)) {
                VS(v, left_sky_x) = a;
                VW(v, left_sky_row) = si;
            }
        }

        /* right edge (widening always) */
        W = RU(v, w, i);
        s16 R = (s16)(RW(v, row_cx, si) + (s16)W);
        if (widen(v, W, &ext)) R = (s16)(R + (s16)ext);
        RW(v, row_r, si) = R;
        a = (s16)(R + bp);
        RW(v, row_or, si) = a;
        s16 c = a;
        hit = false;
        if (st & 0x88) {
            if (st & 0x80) a = (s16)(a - bp);           /* row_r */
            if (a < VS(v, right_cut_x)) {
                VS(v, right_cut_x) = a;
                VW(v, right_cut_row) = si;
                VB(v, right_cut_state) = st;
                hit = true;
            }
        }
        if (!hit) {
            a = RW(v, row_or, si);
            if (si != 0 && a > RW(v, row_or, si - 2) && RW(v, row_sy, si) <= VS(v, top_sy)
                && a > VS(v, right_sky_x)) {
                VS(v, right_sky_x) = a;
                VW(v, right_sky_row) = si;
            }
        }

        /* far-right ground band (DAT+0x33C) */
        {
            u16 b = 0;
            u16 dx = (u16)(VW(v, walk_ptr) - 0x3B33);
            bool found = false;
            u16 bpe = 0;
            while (DSW((u16)(DS_right_zones + b)) != 0) {
                bpe = DSW((u16)(DS_right_zones + 2 + b));
                if (dx <= bpe) { found = true; break; }
                b = (u16)(b + 8);
            }
            u16 start = DSW((u16)(DS_right_zones + b));
            if (found && dx >= start) {
                dx = (u16)(dx - start);
                bpe = (u16)(bpe - start);
                s16 v1 = DSS((u16)(DS_right_zones + 6 + b));
                s16 v0 = DSS((u16)(DS_right_zones + 4 + b));
                s16 d = (s16)(v1 - v0);
                s32 prod = (s32)d * (s16)dx;            /* imul dx */
                s16 q = idiv32_16(prod, (s16)bpe, NULL); /* may fault (R6003), as the original */
                q = (s16)(q + v0);
                c = (s16)(c + mid16((u16)q, RU(v, w, i)));
            } else {
                c = (s16)v->band_default;
            }
            RW(v, row_band, si) = c;
        }

        /* tunnel rows */
        {
            s16 ys = RW(v, row_sy, si);
            s32 tt = (s32)ys - (s16)(RU(v, w, i) >> 1);
            s16 tp = tt > 0 ? (s16)tt : 0;              /* neg bx ; add bx,ax ; jg */
            if (ys >= v->height) ys = v->height;
            if ((s8)(u8)VW(v, rec_w0) < 0) {
                if ((s8)VB(v, r0_state) >= 0) {
                    VW(v, tunnel_out_row) = si;
                    VS(v, tunnel_out_sy) = ys;
                    VS(v, tunnel_out_top) = tp;
                } else {
                    VW(v, tunnel_in_row) = si;
                    VS(v, tunnel_in_sy) = ys;
                    VS(v, tunnel_in_top) = tp;
                }
            }
            if ((VB(v, r0_state) & 0x80) && tp >= VS(v, tunnel_ceiling)) {
                VS(v, tunnel_ceiling) = tp;
                VW(v, tunnel_ceiling_row) = si;
            }
        }
    }
    RW(v, row_clip, 0) = v->height;
}

/* 06c9:06c3 interp_span: stack (a, b, dst), CX = n, DX = dy; the self-modified inc/dec is `step`. */
void interp_span(s16 a, s16 b, u16 dst_ds, u16 cx, u16 dx)
{
    u16 ax = (u16)a, di = dst_ds;
    s16 lim = DSS(DS_view_width);
    if (a <= 0 && b <= 0) {
        ax = 0;
        goto fill;
    }
    if (a >= lim && b >= lim) {
        ax = (u16)lim;
        goto fill;
    }
    DSW(di) = ax; di = (u16)(di + 2);
    cx--;
    {
        s32 d = (s32)b - a;
        if ((u16)d == 0) goto fill;
        u16 step, ad;
        if (d > 0) { step = 1; ad = (u16)d; }           /* jle after sub: true signed result */
        else { step = (u16)-1; ad = (u16)-(u16)d; }
        if (ad > dx) {
            u16 si = 0;
            do {
                do {
                    si = (u16)(si + dx);
                    ax = (u16)(ax + step);
                } while ((s16)si < (s16)ad);
                si = (u16)(si - ad);
                DSW(di) = ax; di = (u16)(di + 2);
            } while (--cx != 0);
        } else {
            u16 si = 0;
            do {
                si = (u16)(si + ad);
                if ((s16)si >= (s16)dx) {
                    ax = (u16)(ax + step);
                    si = (u16)(si - dx);
                }
                DSW(di) = ax; di = (u16)(di + 2);
            } while (--cx != 0);
        }
        return;
    }
fill:
    while (cx--) { DSW(di) = ax; di = (u16)(di + 2); }
}

/* 06c9:05f8 build_spans_front / 06c9:2477 build_spans_mirror — §4.5 */
void build_spans(const SceneView *v)
{
    u16 bp = 0;
    for (u16 si = 2; si != v->nrows2; si += 2) {
        s16 y = RW(v, row_sy, si);
        if (y == RW(v, row_sy, bp)) continue;
        s16 n = (s16)(y - RW(v, row_clip, si - 2));
        if (n < 0) {
            u16 cx = (u16)-n;
            u16 di = (u16)(y << 1);
            if ((u8)cx == 1) {
                SPAN(span_ol, di)    = RW(v, row_ol, si);
                SPAN(span_l, di)     = RW(v, row_l, si);
                SPAN(span_r, di)     = RW(v, row_r, si);
                SPAN(span_or, di)    = RW(v, row_or, si);
                SPAN(span_band, di)  = RW(v, row_band, si);
                SPAN(span_flags, di) = RW(v, row_state, si);
            } else {
                u16 dy = (u16)(RW(v, row_sy, bp) - y);
                u16 fl = RU(v, row_state, si);
                u16 p = (u16)(DS_span_flags + di);
                for (u16 k = cx; k != 0; k--) { DSW(p) = fl; p = (u16)(p + 2); }
                /* called in reverse push order: band, or, r, l, ol */
                interp_span(RW(v, row_band, si), RW(v, row_band, bp), (u16)(DS_span_band + di), cx, dy);
                interp_span(RW(v, row_or, si),   RW(v, row_or, bp),   (u16)(DS_span_or + di),   cx, dy);
                interp_span(RW(v, row_r, si),    RW(v, row_r, bp),    (u16)(DS_span_r + di),    cx, dy);
                interp_span(RW(v, row_l, si),    RW(v, row_l, bp),    (u16)(DS_span_l + di),    cx, dy);
                interp_span(RW(v, row_ol, si),   RW(v, row_ol, bp),   (u16)(DS_span_ol + di),   cx, dy);
            }
        }
        bp = si;
    }
}

static s16 clamp0w(s16 d, s16 w) { return d <= 0 ? 0 : (d < w ? d : w); }

/* 06c9:0746 fix_cut_lines_front / 06c9:2542 fix_cut_lines_mirror — §4.5 */
void fix_cut_lines(const SceneView *v)
{
    u8 bl = VB(v, r0_any);
    if (bl == 0) return;
    s16 wd = v->width;
    if (bl & 0x20) {
        VW(v, left_sky_sy2) = (u16)(RW(v, row_sy, VW(v, left_sky_row)) << 1);
        s16 d = VS(v, left_sky_x);
        if (d < 0) VS(v, left_sky_x) = 0;
        else if (d > wd) VS(v, left_sky_x) = wd;
    }
    if (bl & 0x04) {
        VW(v, right_sky_sy2) = (u16)(RW(v, row_sy, VW(v, right_sky_row)) << 1);
        s16 d = VS(v, right_sky_x);
        if (d < 0) VS(v, right_sky_x) = 0;
        else if (d > wd) VS(v, right_sky_x) = wd;
    }
    {
        s16 d = VS(v, left_cut_x);
        if ((bl & 0x40) && !(VB(v, left_cut_state) & 0x80)) d = (s16)(d - v->cut_offset);
        VS(v, left_cut_x) = clamp0w(d, wd);
        d = VS(v, right_cut_x);
        if ((bl & 0x08) && !(VB(v, right_cut_state) & 0x80)) d = (s16)(d + v->cut_offset);
        VS(v, right_cut_x) = clamp0w(d, wd);
    }
    if (!(bl & 0x80)) return;
    if (VS(v, tunnel_out_sy) == 0) {
        s16 t = VS(v, top_sy);
        VS(v, tunnel_out_sy) = t;
        VS(v, tunnel_out_top) = t;
        VW(v, tunnel_out_row) = v->out_row_default;
    }
    {
        s16 di = VS(v, tunnel_ceiling_row), bp = VS(v, top_row);
        if (di != bp) {
            s16 dx = VS(v, tunnel_ceiling), top = VS(v, top_sy);
            if (di < bp) {
                if (dx > top) VS(v, top_sy) = dx;
            } else {
                if (top < dx) VS(v, tunnel_ceiling) = dx;   /* writes the value it already holds */
            }
        }
    }
    {
        s16 di = VS(v, left_cut_row), bp = VS(v, right_cut_row);
        if (di != bp) {
            s16 dx = VS(v, left_cut_x), bx = VS(v, right_cut_x);
            if (di < bp) {
                if (bx <= dx) VS(v, right_cut_x) = dx;
            } else {
                if (dx >= bx) VS(v, left_cut_x) = bx;
            }
        }
    }
    u16 si = VW(v, tunnel_in_row);
    VS(v, tunnel_in_l) = clamp_edge(v, RW(v, row_l, si), si);
    VS(v, tunnel_in_r) = clamp_edge(v, RW(v, row_r, si), si);
    si = VW(v, tunnel_out_row);
    VS(v, tunnel_out_l) = clamp_edge(v, RW(v, row_l, si), si);
    VS(v, tunnel_out_r) = clamp_edge(v, RW(v, row_r, si), si);
}

/* 06c9:08cb clamp_edge_front / 06c9:268a clamp_edge_mirror: AX = x, SI = row * 2 -> AX */
s16 clamp_edge(const SceneView *v, s16 ax, u16 si)
{
    u8 f = VB(v, r0_any);
    s16 wd = v->width;
    if (f & 0x88) {
        if (ax < 0) ax = 0;
        else if ((s16)si > VS(v, right_cut_row)) { if (ax > VS(v, right_cut_x)) ax = VS(v, right_cut_x); }
        else if (ax > wd) ax = wd;
    }
    if (f & 0xC0) {
        if (ax > wd) return wd;
        if ((s16)si < VS(v, left_cut_row)) return ax;
        if (ax < VS(v, left_cut_x)) return VS(v, left_cut_x);
    }
    return ax;
}

/* 06c9:088e clamp_spans: AX = first scanline, BP = end; clamps the five span arrays to
 * [0, view_width]. The arrays are 92 entries long in both views. */
void clamp_spans(u16 ax_first, u16 bp_end)
{
    /* PORT: the original loops with `loop` (CX = end - first): first == end would run 65536 times over
     * DGROUP and first > end almost as long. top_sy is always < end with the shipped data; the port
     * treats those cases as empty. */
    if ((s16)ax_first >= (s16)bp_end) return;
    s16 bx = DSS(DS_view_width);
    u16 si = DS_span_ol;
    for (int k = 0; k < 5; k++) {
        u16 p = (u16)(si + 2 * ax_first);
        for (u16 cx = (u16)(bp_end - ax_first); cx != 0; cx--) {
            s16 a = DSS(p);
            if (a < 0) DSS(p) = 0;
            else if (a > bx) DSS(p) = bx;
            p = (u16)(p + 2);
        }
        si = (u16)(si + 0xB8);
    }
}
