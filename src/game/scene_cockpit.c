/* scene_render: cockpit — dash loading, per-life reset, HUD (mirror frame, ticket, radar detector,
 * progress dots, distance and time digits), gear gate, steering wheel and marker, instrument cluster.
 * Port of TD2EGA 06c9:37d0..3efb. port/spec/scene_render.md §4.2, §4.12. */
#include "scene.h"
#include "../platform/res.h"

/* 06c9:37d0 cockpit_load — §4.2 */
void cockpit_load(void)
{
    res_find_list_opt(ds_far(DS_dash_arc), 0x304C, DS_dash_handles);          /* dash..hdcM */
    DSB(DS_marker_saved) = 0;
    DSW(DS_wheel_pose) = 2;
    if (CARW(0x14D) != 1)
        res_find_list_opt(ds_far(DS_dash_arc), 0x30B1, DS_dash_digit_handles); /* dgt0..tach */
    FarPtr spr = hnd(DASH_H(DASH_inst));
    FarPtr sp;
    DSW(0x2F74) = rd16(spr.seg, (u16)(spr.off + 8));
    DSW(0x2F76) = rd16(spr.seg, (u16)(spr.off + 0x0A));
    ds_far_wr(DS_inst_desc, create_buffer_like(spr, &sp));
    ds_far_wr(DS_inst_sprite, sp);
    spr = hnd(DASH_H(DASH_gbox));
    DSW(0x2F70) = rd16(spr.seg, (u16)(spr.off + 8));
    DSW(0x2F72) = rd16(spr.seg, (u16)(spr.off + 0x0A));
    ds_far_wr(DS_gbox_desc, create_buffer_like(spr, &sp));
    ds_far_wr(DS_gbox_sprite, sp);
    select_screen();
    blit_copy_own(hnd(DASH_H(DASH_dash)));
    blit_copy_own(hnd(DASH_H(DASH_roof)));
    gfx_draw_line(0, 0, 0x13F, 0, 0);                     /* progress line */
    gfx_plot((s16)(DSW(DS_stage_length) >> 5), 0, 15);  /* stage end marker */
    DSW(DS_dot_x) = 0xFFFF;
    DSW(DS_dot_x + 2) = 0xFFFF;
    DSW(DS_dot_x + 4) = 0xFFFF;
}

/* 06c9:38d4 cockpit_free */
void cockpit_free(void)
{
    gfx_free_buffer(ds_far(DS_gbox_desc));
    gfx_free_buffer(ds_far(DS_inst_desc));
}

/* 06c9:38f2 cockpit_reset */
void cockpit_reset(void)
{
    DSB(DS_gear) = 0;                                     /* DS:2F56 */
    DSW(DS_steer_angle) = 0;
    DSW(DS_view_yaw) = 0;
    DSW(DS_yaw) = 0;
    DSB(0x2F57) = 1;
    DSB(DS_redraw_gate) = 1;
    DSB(DS_redraw_inst) = 1;
    DSB(DS_redraw_hud) = 1;
    DSB(DS_gate_close_delay) = 10;
    DSW(DS_knob_x) = CARW(0x20);
    DSW(DS_knob_y) = CARW(0x22);
}

/* 06c9:3af9 draw_ticket (target: main view) */
static void draw_ticket(void)
{
    u8 al = DSB(DS_cop_state);
    if (al < 4 || al > 6) return;
    blit_copy_own(hnd(ROAD_H(ROAD_tick)));
    u8 v = DSB(DS_ticket_amount);
    u16 hun = (u16)(v / 100);
    u8 rem = (u8)(v % 100);
    u16 ten = (u16)(rem / 10);
    u16 one = (u16)(rem % 10);
    if (hun != 0) {
        blit_xor_raw(hnd((u16)(ROAD_H(ROAD_dgt0) + (hun << 2))), 0x37, 0x13);
    } else if (ten == 0) {
        goto ones;
    }
    blit_xor_raw(hnd((u16)(ROAD_H(ROAD_dgt0) + (ten << 2))), 0x3C, 0x13);
ones:
    blit_xor_raw(hnd((u16)(ROAD_H(ROAD_dgt0) + (one << 2))), 0x41, 0x13);
}

/* 06c9:3a5f draw_radar_detector (target: screen) */
static void draw_radar_detector(void)
{
    if (DSW(DS_sim_tick10) & 4)                           /* DS:3346 radar flags */
        blit_copy_own(hnd((u16)(DASH_H(DASH_rad0) + (u16)(DSW(DS_radar_level) << 2))));
    else
        blit_copy_own(hnd(DASH_H(DASH_radb)));
}

/* 06c9:3ab8 progress_dot: SI = road pointer, BX = slot * 2, DX = colour */
static void progress_dot(u16 si, u16 bx, u8 dx)
{
    s16 x = (s16)si < 0x3B51 ? -32 : (s16)(si - 0x3B51);
    s16 old = DSS((u16)(DS_dot_x + bx));
    x = (s16)(x >> 5);
    if (x == old) return;
    DSS((u16)(DS_dot_x + bx)) = x;
    if (old >= 0) gfx_plot(old, 0, 0);
    if (x >= 0) gfx_plot(x, 0, dx);
}

/* 06c9:3a91 draw_progress_dots */
static void draw_progress_dots(void)
{
    progress_dot(DSW(DS_player_pos), 0, 12);
    progress_dot(DSW(DS_opp_pos), 2, 9);
    progress_dot(DSW(DS_cop_pos), 4, 14);
}

/* digit handle k of ROAD (dgt0..9), index as the original computes it (may be out of range) */
static FarPtr road_digit(u16 k) { return hnd((u16)(ROAD_H(ROAD_dgt0) + (u16)(k << 2))); }

/* 06c9:391f draw_hud — §4.12 */
void draw_hud(void)
{
    select_main_view();
    blit_and_own(hnd(DASH_H(DASH_mirr)));
    draw_ticket();
    select_screen();
    draw_radar_detector();
    draw_progress_dots();
    if (DSB(DS_redraw_hud) == 0) return;
    DSB(DS_redraw_hud) = 0;
    blit_copy_own(hnd(DASH_H(DASH_time)));

    /* distance left / 42: hundreds, tens, ones */
    s16 d = DSS(DS_distance_left);
    if (d <= 0) d = 0;
    u16 ax = div32_16((u32)(u16)d, 42, NULL);
    ax = div16_8(ax, 100);                                /* faults for d >= 25600 * 42, as the original */
    u8 r = (u8)(ax >> 8);
    u16 si = (u16)(s16)(s8)(u8)ax;                        /* cwde */
    ax = div16_8(r, 10);
    u16 bx = (u16)(u8)(ax >> 8);                          /* ones; BH = AH of the CWDE below (0) */
    u16 di = (u16)(s16)(s8)(u8)ax;                        /* tens */
    bx = (u16)(bx | (di & 0xFF00));
    u16 bp = CARW(0xC3);
    blit_or_raw(road_digit(bx), (s16)CARW(0xC9), (s16)bp);
    blit_or_raw(road_digit(di), (s16)CARW(0xC7), (s16)bp);
    blit_or_raw(road_digit(si), (s16)CARW(0xC5), (s16)bp);

    /* time: minutes and seconds */
    u16 sec;
    u16 m = div32_16((u32)(s32)DSS(DS_stage_time), 60, &sec);    /* cdq: signed dividend */
    ax = div16_8(m, 10);
    bx = (u16)(u8)(ax >> 8);                              /* m % 10 */
    di = (u16)(s16)(s8)(u8)ax;                            /* m / 10 (cwde) */
    bx = (u16)(bx | (di & 0xFF00));
    u16 m10 = di, m1 = bx;
    ax = div16_8(sec, 10);
    bx = (u16)(u8)(ax >> 8);
    di = (u16)(s16)(s8)(u8)ax;
    bx = (u16)(bx | (di & 0xFF00));
    blit_or_raw(road_digit(bx), (s16)CARW(0xD1), (s16)bp);
    blit_or_raw(road_digit(di), (s16)CARW(0xCF), (s16)bp);
    blit_or_raw(road_digit(m1), (s16)CARW(0xCD), (s16)bp);
    blit_or_raw(road_digit(m10), (s16)CARW(0xCB), (s16)bp);
}

/* 06c9:3b95 draw_gear_gate — §4.12 (TD1 0x351A) */
void draw_gear_gate(void)
{
    u8 vis = DSB(DS_dash_toggle);                         /* DS:2F58 gate visible */
    if (vis != DSB(DS_gate_prev)) {
        DSB(DS_gate_prev) = vis;
        if (vis == 1) goto redraw;
        goto closed;
    }
    if (DSB(DS_dash_toggle) == 1) goto check;
    if (DSB(DS_gate_close_delay) == 0) return;
    if (--DSB(DS_gate_close_delay) != 0) goto check;
closed:
    select_screen();
    blit_copy_own(hnd(DASH_H(DASH_gbo0)));
    return;
check:
    if (DSB(DS_redraw_gate) != 1) return;
redraw:
    DSB(DS_redraw_gate) = 0;
    gfx_select_target(far_make(ASM_SEG, DSW(DS_gbox_desc)));     /* push cs ; push [2F60] */
    blit_copy_raw(hnd(DASH_H(DASH_gbox)), 0, 0);
    blit_and_clip_hot(hnd(DASH_H(DASH_gnab)), DSS(DS_knob_x), DSS(DS_knob_y));
    blit_or_clip_hot(hnd(DASH_H(DASH_gnob)), DSS(DS_knob_x), DSS(DS_knob_y));
    select_screen();
    blit_copy_own(ds_far(DS_gbox_sprite));
}

/* 06c9:3c3a draw_steering — §4.12 */
void draw_steering(void)
{
    FarPtr save = ds_ptr(DS_marker_save_sprite);
    if (DSB(DS_marker_saved) != 0)
        blit_copy_raw(save, DSS(DS_marker_save_x), DSS(DS_marker_save_y));
    u16 old = DSW(DS_wheel_pose);
    u16 di = 2;
    s8 ah = (s8)(DSW(DS_steer_angle) >> 8);
    if (ah >= 4) di = (u16)(di - 2);
    if (ah <= -4) di = (u16)(di + 2);
    if (di != old) {
        DSW(DS_wheel_pose) = di;
        if (di == 2) di = old;                            /* back to centre: remove the old pose */
        blit_xor_own(hnd((u16)(DASH_H(DASH_whl1) + di)));   /* whl1 (0) / whl3 (4) */
    }
    u16 ax = (u16)((u16)(0x0F00 - DSW(DS_steer_angle)) << 1);
    ax = (u16)(s16)(s8)(u8)(ax >> 8);                     /* cwde */
    u16 tip = CARW((u16)(0xD3 + (u16)(ax << 1)));
    u16 dx = (u16)((ax & 0xFF00) | (tip & 0xFF));         /* mov dh,ah */
    /* TODO(verify): CH is left over from the previous library call (scene_render.md §9 item 1); the
     * port assumes 0. */
    u16 cx = (u16)(tip >> 8);
    DSB(DS_marker_saved) = (u8)cx;
    u16 sy = (u16)(cx - 2);
    u16 sx = (u16)((dx - 2) & 0xFFF8);
    DSW(DS_marker_save_y) = sy;
    DSW(DS_marker_save_x) = sx;
    grab_into_sprite_raw(save, (s16)sx, (s16)sy);
    blit_and_hot(hnd(DASH_H(DASH_dota)), (s16)dx, (s16)cx);
    blit_or_hot(hnd(DASH_H(DASH_dot)), (s16)dx, (s16)cx);
}

/* 06c9:3ed7 draw_inst_digit: AL = digit, SI = slot * 2 */
static void draw_inst_digit(u8 al, u16 *si)
{
    blit_or_clip_raw(hnd((u16)(DIG_H(0) + ((u16)al << 2))), (s16)CARW((u16)(0x165 + *si)), (s16)CARW(0x163));
    *si = (u16)(*si + 2);
}

/* 06c9:3ea4 draw_number3: AX = value, SI = slot * 2 */
static void draw_number3(u16 ax, u16 *si)
{
    ax = div16_8(ax, 100);
    u8 r = (u8)(ax >> 8);
    if ((u8)ax != 0) {
        draw_inst_digit((u8)ax, si);
        ax = div16_8(r, 10);
        draw_inst_digit((u8)ax, si);
        draw_inst_digit((u8)(ax >> 8), si);
        return;
    }
    *si = (u16)(*si + 2);
    ax = div16_8(r, 10);
    if ((u8)ax != 0) {
        draw_inst_digit((u8)ax, si);
        draw_inst_digit((u8)(ax >> 8), si);
        return;
    }
    *si = (u16)(*si + 2);
    draw_inst_digit((u8)(ax >> 8), si);
}

/* 06c9:3cef draw_instruments — §4.12 */
void draw_instruments(void)
{
    if (DSB(DS_redraw_inst) != 1) return;
    DSB(DS_redraw_inst) = 0;
    gfx_select_target(far_make(ASM_SEG, DSW(DS_inst_desc)));     /* push cs ; push [2F68] */
    gfx_clear_clip(0);
    /* TODO(verify): SI is the stage runner's SI when the speed is not shown with digits; only reached
     * by cars with rpm digits but no speed digits (none shipped: VETT has 0x0002, the others 0). The
     * port starts at slot 0. */
    u16 si = 0;
    u16 f = CARW(0x14D);
    if (f & 3) {
        if (f & 1) {                                      /* speed bar */
            u16 ax = (u16)(DSB(0x52CF) - CARW(0x15B));
            if (ax >= CARW(0x153)) ax = CARW(0x153);
            ax = (u16)(u8)div16_8((u16)(ax << 1), CARB(0x157));
            s16 cx = CARS(0x175), dx = CARS(0x177);
            u16 bx = CARW(0x15D);
            if (CARB(0x14F) == 0) {                       /* vertical */
                u16 t = bx; bx = ax; ax = t;
                dx = (s16)(dx - (s16)bx);
            }
            gfx_fill_rect(cx, dx, (s16)ax, (s16)bx, 0xFF);
            blit_and_clip_own(hnd(DIG_H(10)));            /* spdo */
            if (!(CARW(0x14D) & 2)) goto tach;
        }
        si = 0;
        draw_number3(DSB(0x52CF), &si);                  /* speed (DS:52CE high byte) */
    } else {                                              /* speed needle */
        u16 q = (u16)(u8)div16_8((u16)(DSB(0x52CF) << 1), 5);
        u16 tip = CARW((u16)(0x179 + (q << 1)));
        gfx_draw_line(CARS(0x175), CARS(0x177), (s16)(tip & 0xFF), (s16)(tip >> 8), 15);
    }
tach:
    if (CARW(0x14D) & 0x0C) {
        if (CARW(0x14D) & 4) {                            /* rpm bar */
            u16 ax = (u16)(DSW(DS_rpm) - CARW(0x15F));
            if (ax >= CARW(0x155)) ax = CARW(0x155);
            ax = (u16)(u8)div16_8(ax, CARB(0x159));
            s16 cx = CARS(0x249), dx = CARS(0x24B);
            u16 bx = CARW(0x161);
            if (CARB(0x151) == 0) {                       /* vertical: subtracts the thickness */
                u16 t = bx; bx = ax; ax = t;
                dx = (s16)(dx - (s16)ax);
            }
            gfx_fill_rect(cx, dx, (s16)ax, (s16)bx, 0xFF);
            blit_and_clip_own(hnd(DIG_H(11)));            /* tach */
            if (!(CARW(0x14D) & 8)) goto done;
        }
        u16 q = div32_16((u32)(s32)DSS(DS_rpm), 100, NULL);    /* cdq: signed dividend */
        draw_number3(q, &si);
    } else {                                              /* rpm needle */
        u16 bx = DSW(DS_rpm);
        if (bx >= CARW(0x002)) bx = CARW(0x002);
        bx = (u16)(bx >> 6);
        if (CARB(0x24D) != 0) bx = (u16)(bx >> 1);
        u16 tip = CARW((u16)(0x24F + (bx << 1)));
        gfx_draw_line(CARS(0x249), CARS(0x24B), (s16)(tip & 0xFF), (s16)(tip >> 8), 15);
    }
done:
    blit_or_clip_raw(hnd(DASH_H(DASH_inst)), 0, 0);
    blit_xor_own(hnd((u16)(DASH_H(DASH_inl1) + (u16)(DSW(DS_wheel_pose) << 1))));
    select_screen();
    blit_copy_own(ds_far(DS_inst_sprite));
}
