/* scene_render: stage runner and per-frame loop, stage / view loading, snapshots, top-level front and
 * mirror drawing (including the falling-off-the-road view), crash / engine smoke sequences and result
 * messages. Port of TD2EGA 06c9:0002..0200, 06c9:1b2c..20ff (runner, loaders, snapshots),
 * 06c9:3532..37cf, 06c9:3efc..3f3f. port/spec/scene_render.md §4.1-§4.3, §4.7, §4.13. */
#include <stdio.h>
#include <string.h>
#include "scene.h"
#include "../host.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/sound.h"
#include "../platform/timer.h"

static void fill(s16 x, s16 y, s16 w, s16 h, u16 colour)       /* 06c9:89a2 */
{
    gfx_fill_rect_clip(x, y, w, h, (u8)colour);
}

/* Road position of a 32-bit pointer pair relative to `me` (sub / sbb): high word, and whether the
 * signed 32-bit difference is >= 0 (the `jge` after `sbb`). */
static u16 rel_hi(u16 lo, u16 hi, u16 me_lo, u16 me_hi, bool *ge)
{
    u32 a = (u32)hi << 16 | lo, b = (u32)me_hi << 16 | me_lo;
    *ge = (int64_t)(s32)a - (int64_t)(s32)b >= 0;
    return (u16)((a - b) >> 16);
}

/* ------------------------------------------------------------------------------------------------ */
/* 06c9:0002 main_view_load — §4.2 */
void main_view_load(void)
{
    FarPtr desc = gfx_create_buffer(320, 0x5C, 0x0F);
    ds_far_wr(DS_drive_buf_desc, desc);
    DSW(DS_main_rowtab) = rd16(desc.seg, (u16)(desc.off + 0x0A));
    FarPtr spr = far_make(rd16(desc.seg, (u16)(desc.off + 2)), rd16(desc.seg, desc.off));
    ds_far_wr(DS_main_sprite, spr);
    wr16(spr.seg, (u16)(spr.off + 8), 0);
    wr16(spr.seg, (u16)(spr.off + 0x0A), 0x13);
    res_find_list_opt(ds_far(DS_scenery_arc), 0x08B2, DS_scenery_sky_handles);   /* rcfA..clo3 */
    res_find_list_opt(ds_far(DS_opp_road_arc), 0x08EB, DS_opp_road_handles);    /* rcr0..brk7 */
    DSW(DS_wide_toggle_ptr) = 0;
    DSB(DS_prev_road_byte) = 0;
    DSW(0x09BC) = 0;
}

/* 06c9:0083 main_view_free */
void main_view_free(void)
{
    gfx_free_buffer(ds_far(DS_drive_buf_desc));
}

/* 06c9:0094 present_main_view */
void present_main_view(void)
{
    select_screen();
    blit_copy_own(ds_far(DS_main_sprite));
}

/* 06c9:3efc select_main_view */
void select_main_view(void)
{
    gfx_select_target(ds_far(DS_drive_buf_desc));
}

/* 06c9:00aa project_front */
void project_front(void)
{
    DSW(DS_view_width) = 320;
    project_rows(&scene_front_view);
    build_spans(&scene_front_view);
    fix_cut_lines(&scene_front_view);
    clamp_spans(DSW(DS_top_sy), 0x5C);
}

/* 06c9:1f71 project_mirror */
void project_mirror(void)
{
    DSW(DS_view_width) = 80;
    project_rows(&scene_mirror_view);
    build_spans(&scene_mirror_view);
    fix_cut_lines(&scene_mirror_view);
    clamp_spans(VW(&scene_mirror_view, top_sy), 0x11);
}

static void cache_plane_segs(void)
{
    for (int k = 0; k < 4; k++)
        DSW((u16)(DS_plane_seg + 2 * k)) = CSW((u16)(GFX_CUR_PLANE0 + 2 * k));
}

/* 06c9:00e7 draw_front — §4.7 */
void draw_front(void)
{
    const SceneView *v = &scene_front_view;
    DSW(DS_row_skip_target) = v->skip_target;
    gfx_select_target(ds_far(DS_drive_buf_desc));
    cache_plane_segs();
    u8 mode = DSB(DS_fall_mode);
    if (mode == 0 || DSW(DS_fall_scroll) < 0x5C) {
        draw_sky(v);
        draw_ground(v);
        if (DSB(DS_r0_any) & 0x80) draw_tunnel_walls(v);
        draw_front_objects();
    }
    if (DSB(DS_fall_mode) == 0) return;
    u16 ax = DSW(DS_fall_scroll);
    if (ax == 0) return;
    if (DSB(DS_fall_mode) == 4) {                         /* water */
        blit_copy_clip_raw(ds_far(DS_main_sprite), 0, (s16)-ax);
        fill(0, (s16)(0x5C - ax), 320, 0xB4, 9);
        return;
    }
    blit_copy_clip_raw(ds_far(DS_main_sprite), 0, (s16)-ax);
    s16 bx;
    u16 cl, cr, cb;
    if (DSB(DS_fall_mode) == 1) {
        bx = DSS(DS_left_sky_x);
        cl = DSW(DS_col_sky); cr = 6; cb = 6;
    } else {
        bx = DSS(DS_right_sky_x);
        cl = 6; cr = DSW(DS_col_sky); cb = 6;
    }
    s16 cy = (s16)(0x5C - DSW(DS_fall_scroll));
    fill(0, (s16)(cy + 0xB4), 320, 100, cb);
    fill(bx, cy, (s16)(320 - bx), 0xB4, cr);
    fill(0, cy, bx, 0xB4, cl);
}

/* 06c9:1f99 draw_mirror — §4.7 */
void draw_mirror(void)
{
    const SceneView *v = &scene_mirror_view;
    DSW(DS_row_skip_target) = v->skip_target;
    gfx_select_target(ds_far(DS_mirror_desc));
    cache_plane_segs();
    if (DSB(DS_fall_mode) == 0 || DSW(DS_fall_scroll) == 0) {
        draw_sky(v);
        draw_ground(v);
        if (VB(v, r0_any) & 0x80) draw_tunnel_walls(v);
        draw_mirror_objects();
    }
    if (DSB(DS_fall_mode) != 0 && DSW(DS_fall_scroll) != 0) {
        u16 ax = (u16)(DSW(DS_fall_scroll) >> 3);
        if (DSB(DS_fall_mode) == 4) {
            blit_copy_clip_raw(ds_far(DS_mirror_sprite), 0, (s16)-ax);
            fill(0, (s16)(0x11 - ax), 0x50, 0xB4, 9);
        } else {
            ax = (u16)(ax + VW(v, top_sy));
            fill(0, (s16)ax, 0x50, (s16)(0x11 - ax), 6);
            fill(0, 0, 0x50, (s16)ax, DSW(DS_col_sky));
        }
    }
    select_main_view();
    blit_copy_own(ds_far(DS_mirror_sprite));
}

/* ------------------------------------------------------------------------------------------------ */
/* 06c9:0201 snapshot_front — §4.3 */
void snapshot_front(void)
{
    memset(mp(DGROUP, DS_pitch_acc), 0, 0x11 * 2);        /* DS:1308..1329 */
    DSW(DS_left_sky_x) = 320;
    DSW(DS_right_cut_x) = 320;
    DSW(DS_tunnel_in_r) = 320;
    DSW(DS_top_sy) = 0x5C;
    DSW(DS_tunnel_in_sy) = 0x5C;
    u8 al = DSB(DS_start_flags);
    DSB(DS_r0_state) = al;
    DSB(DS_r0_any) = al;
    DSB(DS_left_cut_state) = al;
    DSB(DS_right_cut_state) = al;
    DSW(DS_unit_phase) = DSW(DS_ring_counter);
    DSW(DS_heading_acc) = DSW(DS_view_yaw);
    DSW(DS_walk_ptr) = (u16)(DSW(DS_player_pos) + 1);
    DSW(DS_walk_sub) = DSW(DS_player_pos_lo);
    DSW(DS_lat_acc) = (u16)-DSW(DS_player_lateral);
    DSB(DS_r_opp_brake) = DSB(DS_opp_braking);
    DSB(DS_r_cop_brake) = DSB(DS_cop_braking);
    DSB(DS_r_opp_alt) = DSB(DS_opp_crash_timer);
    DSW(DS_height_acc) = DSW(DS_start_height_front);
    u16 bx = DSW(DS_walk_sub), cx = DSW(DS_walk_ptr);
    u16 di = 0;
    bool ge;
    for (int l = 0; l < 2; l++) {
        u16 list = l == 0 ? DS_oncoming : DS_same_dir;
        u16 len = l == 0 ? DSW(DS_oncoming_count8) : DSW(DS_same_count8);
        u16 si = 0;
        do {
            u16 bp = DSW((u16)(list + si));
            u16 dx = rel_hi(DSW((u16)(list + si + 4)), DSW((u16)(list + si + 2)), bx, cx, &ge);
            if (!ge) dx = (u16)(dx + DSW(DS_dat_road_units));
            if (dx <= 0x3C) {
                DSW((u16)(DS_front_draw_list + di)) = (u16)(dx << 1);
                DSW((u16)(DS_front_draw_list + 2 + di)) = bp;
                DSW((u16)(DS_front_draw_list + 4 + di)) = DSW((u16)(list + si + 6));
                di = (u16)(di + 8);
            }
            si = (u16)(si + 8);
        } while (si < len);                               /* do-while: the first entry is always read */
    }
    DSW(DS_front_draw_list_len) = di;
    u16 dx = rel_hi(DSW(DS_opp_pos_lo), DSW(DS_opp_pos), bx, cx, &ge);
    if (!ge) dx = (u16)(dx + DSW(DS_dat_road_units));
    DSW(DS_opp_row2) = (u16)(dx << 1);
    DSW(DS_opp_lat) = DSW(DS_opp_lateral);
    dx = rel_hi(DSW(DS_cop_pos_lo), DSW(DS_cop_pos), bx, cx, &ge);
    if (!ge) dx = (u16)(dx + DSW(DS_dat_road_units));
    DSW(DS_cop_row2) = (u16)(dx << 1);
    DSW(DS_cop_lat) = DSW(DS_cop_lateral);
    DSB(DS_r_cop_active) = DSB(DS_cop_active);
    DSB(DS_r_cop_state) = DSB(DS_cop_state);
}

/* mirror distance: -hi(obj - me), + road units when that is negative (neg dx ; jge) */
static u16 mirror_dist(u16 lo, u16 hi, u16 me_lo, u16 me_hi)
{
    bool ge;
    u16 dx = rel_hi(lo, hi, me_lo, me_hi, &ge);
    bool pos = (s16)dx > 0;                               /* jge after neg: taken for dx <= 0 */
    dx = (u16)-dx;
    if (pos) dx = (u16)(dx + DSW(DS_dat_road_units));
    return dx;
}

/* 06c9:2089 snapshot_mirror — §4.3 */
void snapshot_mirror(void)
{
    const SceneView *v = &scene_mirror_view;
    memset(mp(DGROUP, 0x2C64), 0, 0x11 * 2);              /* DS:2C64..2C85 */
    VW(v, left_sky_x) = 0x50;
    VW(v, right_cut_x) = 0x50;
    VW(v, tunnel_in_r) = 0x50;
    VW(v, top_sy) = 0x11;
    VW(v, tunnel_in_sy) = 0x11;
    u8 al = DSB(DS_start_flags);
    VB(v, r0_state) = al;
    VB(v, r0_any) = al;
    VB(v, left_cut_state) = al;
    VB(v, right_cut_state) = al;
    DSW(DS_unit_phase_m) = (u16)(DSW(DS_ring_counter) - 1);
    VW(v, heading_acc) = (u16)-DSW(DS_view_yaw);
    VW(v, walk_ptr) = DSW(DS_player_pos);
    VW(v, walk_sub) = DSW(DS_player_pos_lo);
    VW(v, lat_acc) = (u16)-(u16)(DSS(DS_player_lateral) >> 1);
    DSB(DS_r_opp_alt_m) = DSB(DS_opp_crash_timer);
    VW(v, height_acc) = DSW(DS_start_height_mirror);
    u16 bx = VW(v, walk_sub), cx = VW(v, walk_ptr);
    u16 di = 0;
    for (int l = 0; l < 2; l++) {
        u16 list = l == 0 ? DS_oncoming : DS_same_dir;
        u16 len = l == 0 ? DSW(DS_oncoming_count8) : DSW(DS_same_count8);
        u16 si = 0;
        do {
            u16 bp = DSW((u16)(list + si));
            u16 dx = mirror_dist(DSW((u16)(list + si + 4)), DSW((u16)(list + si + 2)), bx, cx);
            if (dx <= 0x19) {
                DSW((u16)(DS_mirror_draw_list + di)) = (u16)(dx << 1);
                DSW((u16)(DS_mirror_draw_list + 2 + di)) = bp;
                DSW((u16)(DS_mirror_draw_list + 4 + di)) = DSW((u16)(list + si + 6));
                di = (u16)(di + 8);
            }
            si = (u16)(si + 8);
        } while (si < len);
    }
    DSW(DS_mirror_draw_list_len) = di;
    DSW(v->opp_row2) = (u16)(mirror_dist(DSW(DS_opp_pos_lo), DSW(DS_opp_pos), bx, cx) << 1);
    DSW(v->opp_lat) = DSW(DS_opp_lateral);
    DSW(v->cop_row2) = (u16)(mirror_dist(DSW(DS_cop_pos_lo), DSW(DS_cop_pos), bx, cx) << 1);
    DSW(v->cop_lat) = DSW(DS_cop_lateral);
    VB(v, r_cop_active) = DSB(DS_cop_active);
    VB(v, r_cop_state) = DSB(DS_cop_state);
}

/* ------------------------------------------------------------------------------------------------ */
/* 06c9:1ee2 mirror_load */
void mirror_load(void)
{
    FarPtr desc = gfx_create_buffer(0x50, 0x11, 0x0F);
    ds_far_wr(DS_mirror_desc, desc);
    DSW(DS_mirror_rowtab) = rd16(desc.seg, (u16)(desc.off + 0x0A));
    FarPtr spr = far_make(rd16(desc.seg, (u16)(desc.off + 2)), rd16(desc.seg, desc.off));
    ds_far_wr(DS_mirror_sprite, spr);
    wr16(spr.seg, (u16)(spr.off + 8), 0xF0);
    wr16(spr.seg, (u16)(spr.off + 0x0A), 8);
    res_find_list_opt(ds_far(DS_scenery_arc), 0x296C, DS_mirror_scenery_handles);  /* rcfC..rmt2 */
    res_find_list_opt(ds_far(DS_opp_road_arc), 0x2999, DS_opp_front_handles);     /* fcr0..fa3M */
    DSB(DS_prev_road_byte_m) = 0;
    DSW(0x2A2A) = 0;
}

/* 06c9:1f60 mirror_free */
void mirror_free(void)
{
    gfx_free_buffer(ds_far(DS_mirror_desc));
}

/* 06c9:1c8f stage_load — §4.2 */
void stage_load(void)
{
    DSW(DS_heading) = 0;                                  /* DS:5344 mountain scroll */
    DSB(DS_start_flags) = 0;
    DSW(DS_distance_left) = DSW(DS_stage_length);
    u16 dx = DSW(DS_dat_road_units);
    DSW(DS_stage_len_copy) = dx;
    DSW(DS_fuel) = (u16)(dx - 10);                        /* DS:3320 */
    DSW(DS_scene_words) = DSW(0x378C);                    /* col_left     DAT+0x322 */
    DSW(DS_col_right) = DSW(0x3790);                      /* col_right    DAT+0x326 */
    DSW(DS_col_shoulder) = DSW(0x3794);                   /* col_shoulder DAT+0x32A */
    DSW(DS_col_sky) = DSW(0x3798);                        /* col_sky      DAT+0x32E */
    DSW(DS_col_far) = DSW(0x379C);                        /* col_far      DAT+0x332 */
    DSW(DS_player_pos) = 0x3B51;
    DSW(DS_opp_pos) = 0x3B51;
    if (DSB(DS_game_mode) == 0) DSW(DS_opp_pos) = 0;      /* DS:843E opponent selected */
    DSW(DS_player_pos_lo) = 0;
    DSW(DS_opp_pos_lo) = 0;
    main_view_load();
    mirror_load();
    res_find_list_opt(ds_far(DS_scenery_arc), 0x13DD, DS_scenery_handles);
    res_find_list(ds_far(DS_road_arc), 0x171E, DS_road_handles);
    res_find_list(ds_far(DS_scn_car1_arc), 0x135C, DS_traffic1_handles);
    res_find_list(ds_far(DS_scn_car2_arc), 0x135C, DS_traffic2_handles);
    res_find_list(ds_far(DS_scn_car3_arc), 0x135C, DS_traffic3_handles);
    res_find_list(ds_far(DS_cop_arc), 0x135C, DS_cop_car_handles);
    res_find_list(ds_far(DS_cop_arc), 0x192F, DS_cop_extra_handles);
    cockpit_load();
    DSW(DS_opp_lateral) = 0xC8;
    DSW(DS_opp_speed) = 0;
    DSW(DS_opp_lateral) = 0xFF38;
    u16 bx = (u16)(DSW(DS_difficulty) << 1);
    DSW(DS_opp_accel_mult) = DSW((u16)(DS_DIFF_ACCEL_MULT + bx));
    DSW(DS_opp_vmax) = DSW((u16)(DS_DIFF_VMAX + bx));
    DSW(DS_opp_min_gap) = DSW((u16)(DS_DIFF_MIN_GAP + bx));
    DSW(DS_opp_curve_factor) = DSW((u16)(DS_DIFF_CURVE_FACTOR + bx));
    DSW(DS_opp_lat_rate) = DSW((u16)(DS_DIFF_LAT_RATE + bx));
    DSW(DS_opp_unused_5360) = DSW((u16)(DS_DIFF_UNUSED + bx));
    bx = (u16)(bx + 6);
    if (bx >= 0x16) bx = 0x16;
    DSW(DS_cop_accel_mult) = DSW((u16)(DS_DIFF_ACCEL_MULT + bx));
    DSW(DS_cop_vmax) = DSW((u16)(DS_DIFF_VMAX + bx));
    DSW(DS_cop_min_gap) = DSW((u16)(DS_DIFF_MIN_GAP + bx));
    DSW(DS_cop_curve_factor) = DSW((u16)(DS_DIFF_CURVE_FACTOR + bx));
    DSW(DS_cop_lat_rate) = DSW((u16)(DS_DIFF_LAT_RATE + bx));
    DSW(DS_cop_unused_536a) = DSW((u16)(DS_DIFF_UNUSED + bx));
    DSW(DS_crashes) = 0;
    DSW(DS_engines_blown) = 0;
    DSW(DS_tickets) = 0;
    DSW(DS_out_of_gas) = 0;
    sim_stage_start();                                    /* 06c9:3f40 */
}

/* 06c9:1e31 life_reset */
void life_reset(void)
{
    sim_restart_reset();                                  /* 06c9:3ff9 */
    DSW(DS_player_lateral) = 0xA0;
    DSB(DS_run_state) = 0;
    DSW(DS_tone_engine) = 0xFFFF;
    DSW(DS_tone_effect) = 0xFFFF;
    DSW(DS_tone_slot3) = 0xFFFF;
    cockpit_reset();
}

/* 06c9:1e59 prepare_traffic_lists — §4.2 */
void prepare_traffic_lists(void)
{
    u16 bp = DS_oncoming;
    DSB(0x1A70) = (u8)-DSB(DS_diff_b);                    /* DS:920C traffic setting */
    for (;;) {
        u16 si = 0;
        while (DSB((u16)(bp + si)) != 0) {
            if (si > 0x10 && rand8() <= DSB(0x1A70)) {
                /* drop the entry: rep movsb of the rest of the 400-byte list */
                u16 n = (u16)(0x190 - (si + 8));
                for (u16 k = 0; k < n; k++)
                    DSB((u16)(bp + si + k)) = DSB((u16)(bp + si + 8 + k));
                continue;                                 /* re-test the same slot */
            }
            u16 ax = div32_16((u32)DSW((u16)(bp + si + 2)) * DSW(DS_dat_road_units), 1000, NULL);
            DSW((u16)(bp + si + 2)) = (u16)(ax + 0x3B51);
            u16 lat = 0xFF38;
            if (bp != DS_oncoming) {
                lat = 0xC8;
                DSB((u16)(bp + si)) = (u8)(DSB((u16)(bp + si)) + 4);
            }
            DSW((u16)(bp + si + 6)) = lat;
            si = (u16)(si + 8);
            if (si >= 0x190) break;
        }
        if (bp == DS_oncoming) {
            DSW(DS_oncoming_count8) = si;
            bp = DS_same_dir;
        } else {
            DSW(DS_same_count8) = si;
            return;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* 06c9:1b2c run_stage — §4.1 */
s16 run_stage(void)
{
    prepare_traffic_lists();
    stage_load();
    gfx_video_hook();
new_life:
    life_reset();
    traffic_resync();                                     /* 06c9:5ada */
    sfx_set_loop(ds_ptr(DS_stream_engine));
    do {
        host_frame_begin();                               /* PORT: emulated frame pacing */
        snapshot_front();
        snapshot_mirror();
        project_front();
        draw_front();
        project_mirror();
        draw_mirror();
        draw_hud();
        present_main_view();
        draw_gear_gate();
        draw_instruments();
        draw_steering();
    } while (DSB(DS_run_state) == 0);
    s8 r = (s8)DSB(DS_run_state);
    DSB(DS_clock_started) = 0;
    if (r > 0) {
        sfx_clear_loop();
        switch (r) {                                      /* jump table DS:2302 */
        case 1: msg_fill_er_up(); goto finish;
        case 2: crash_sequence(); break;
        case 3: engine_smoke_sequence(); break;
        case 4: DSW(DS_out_of_gas)++; msg_missed_gas(); break;
        case 5: msg_engine_dead(); break;
        case 6: msg_suspension_dead(); break;
        case 7: msg_steering_dead(); break;
        case 8: msg_too_much_damage(); break;
        case 9: msg_too_far_left(); break;
        default:
            /* PORT: the original jumps through DS:2302 beyond its 10 entries; the simulation only
             * writes 1..9. */
            fatal("run_stage: bad drive result %d", r);
        }
        if (--DSW(DS_lives) == 0) {
            select_screen();
            blit_and_own(hnd(ROAD_H(ROAD_GOVR)));
            blit_or_own(hnd(ROAD_H(ROAD_govr)));
            wait_after_message();
            DSB(DS_run_state) = 0;
            goto finish;
        }
        msg_lives_left();
        if (DSB(DS_run_state) == 4 || DSB(DS_run_state) == 9) goto finish;
        goto new_life;
    }
finish:
    {
        u16 bx = (u16)(DSW(DS_stage_length) - 0x0B);
        u16 ax = (u16)(DSW(DS_player_pos) - 0x3B51);
        DSW(DS_player_dist) = ax < bx ? ax : bx;
        ax = (u16)(DSW(DS_opp_pos) - 0x3B51);
        DSW(DS_opp_dist) = ax < bx ? ax : bx;
    }
    DSW(DS_stage_time) = DSW(DS_race_time);
    sfx_clear_loop();
    mirror_free();
    main_view_free();
    cockpit_free();
    timer_install_drive();
    /* PORT: 06c9:1bf7 is patched to `retf` by the copy protection (passed state) */
    return (s16)(s8)DSB(DS_run_state);
}

/* ------------------------------------------------------------------------------------------------ */
/* 06c9:3532 crash_sequence — §4.13 */
void crash_sequence(void)
{
    sfx_play(ds_ptr(DS_stream_noise_long));
    snapshot_front();
    snapshot_mirror();
    project_front();
    draw_front();
    project_mirror();
    draw_mirror();
    select_main_view();
    blit_and_clip_own(hnd(DASH_H(DASH_hdcM)));
    blit_or_clip_own(hnd(DASH_H(DASH_hdcr)));
    for (u16 di = 0; di != 7; di++) {
        deadline_set(10);
        select_main_view();
        DSB(DS_seq_counter) = DSB((u16)(DS_crack_line_counts + di));
        u16 bp = DSW((u16)(DS_crack_line_ptrs + 2 * di));
        u16 si = 0;
        do {
            /* point word: low byte x/2, high byte screen y. AH of the second point's CWDE is reused
             * for the first point (no CWDE there). */
            u16 pb = DSW((u16)(bp + si + 2));
            u16 ax = (u16)(s16)(s8)(u8)((pb >> 8) - 0x13);
            u8 ah = (u8)(ax >> 8);
            s16 x1 = (s16)(u16)((((u16)ah << 8) | (pb & 0xFF)) << 1);
            s16 y1 = (s16)ax;
            u16 pa = DSW((u16)(bp + si));
            s16 y0 = (s16)(u16)(((u16)ah << 8) | (u8)((pa >> 8) - 0x13));
            s16 x0 = (s16)(u16)((((u16)ah << 8) | (pa & 0xFF)) << 1);
            gfx_draw_line(x0, y0, x1, y1, 0xFF);
            si = (u16)(si + 4);
        } while (--DSB(DS_seq_counter) != 0);
        gfx_set_palette((di & 1) ? DS_pal_normal : DS_pal_flash);
        deadline_wait();
        draw_mirror();
        draw_hud();
        present_main_view();
    }
    gfx_set_palette(DS_pal_normal);
    gfx_video_hook();
    DSW(DS_crashes)++;
    delay_ticks(60);
}

/* 06c9:3643 engine_smoke_sequence — §4.13 */
void engine_smoke_sequence(void)
{
    sfx_play(ds_ptr(DS_stream_noise_long));
    DSB(DS_seq_counter) = 10;
    select_screen();
    u16 di = 0;
    do {
        kbd_flush();
        deadline_set(40);
        if (di < 0x0C) {
            blit_and_own(hnd((u16)(ROAD_H(ROAD_SMK0) + di)));
            blit_or_own(hnd((u16)(ROAD_H(ROAD_smk0) + di)));
        } else {
            blit_copy_own(hnd((u16)(ROAD_H(ROAD_smk0) + di)));
        }
        blit_and_hot(hnd(DASH_H(DASH_mirr)), 0xF0, 0x1B);
        di = (u16)(di + 4);
        if (di >= 0x18) di = 0x0C;
        deadline_wait();
    } while (--DSB(DS_seq_counter) != 0);
    DSW(DS_engines_blown)++;
}

/* 06c9:3788 message_box */
void message_box_06C9_3788(void)
{
    select_screen();
    gfx_set_text_colours(15, 0);
    gfx_fill_rect(30, 30, 0x105, 0x33, 0);
    draw_rect_outline(0x21, 0x21, 0x11F, 0x4D, 0xFFFF);
}

/* 06c9:362f message_wait_tail */
void message_wait_tail(void)
{
    delay_ticks(30);
    wait_after_message();
}

static void msg_one(u16 text_ds)                          /* 06c9:3703 */
{
    draw_text_centered(text_ds, 0x32);
    message_wait_tail();
}

/* 06c9:36e0 msg_missed_gas */
void msg_missed_gas(void)
{
    message_box_06C9_3788();
    draw_text_centered(0x2E2A, 0x2A);                     /* "You missed the gas station" */
    draw_text_centered(0x2E5C, 0x3A);                     /* "Hope you enjoy the walk." */
    msg_one(0x2E45);                                      /* "and you're out of gas." */
}

/* 06c9:3713 msg_too_far_left */
void msg_too_far_left(void)
{
    message_box_06C9_3788();
    DSW(DS_out_of_gas)++;
    msg_one(0x2E75);                                      /* "Too far left to reach pump." */
}

/* 06c9:371f msg_engine_dead */
void msg_engine_dead(void)
{
    message_box_06C9_3788();
    DSW(DS_crashes)++;
    msg_one(0x2E91);                                      /* "Engine has lost all power..." */
}

/* 06c9:372b msg_suspension_dead */
void msg_suspension_dead(void)
{
    message_box_06C9_3788();
    DSW(DS_crashes)++;
    msg_one(0x2EAE);                                      /* "Suspension completely gone..." */
}

/* 06c9:3737 msg_steering_dead */
void msg_steering_dead(void)
{
    message_box_06C9_3788();
    DSW(DS_crashes)++;
    msg_one(0x2ECC);                                      /* "Steering completely shot..." */
}

/* 06c9:3743 msg_too_much_damage */
void msg_too_much_damage(void)
{
    message_box_06C9_3788();
    DSW(DS_crashes)++;
    msg_one(0x2EE8);                                      /* "Car took too much damage..." */
}

/* 06c9:374f msg_fill_er_up: nothing on the scenery's last stage */
void msg_fill_er_up(void)
{
    if (DSW(DS_last_stage) != 0) return;
    message_box_06C9_3788();
    msg_one(0x2F04);                                      /* "Fill 'er up..." */
}

/* 06c9:375f msg_lives_left */
void msg_lives_left(void)
{
    message_box_06C9_3788();
    if (DSW(DS_lives) == 1) {
        msg_one(0x2F27);                                  /* "Careful, it's your last life!" */
        return;
    }
    /* sprintf(DS:2F20, "%d", lives): overwrites the tail of "Lives left:     " (DS:2F13) */
    char buf[8];
    int n = snprintf(buf, sizeof buf, "%d", (int)DSS(DS_lives));
    memcpy(mp(DGROUP, 0x2F20), buf, (size_t)n + 1);
    msg_one(0x2F13);
}

/* 06c9:3f0d wait_after_message */
void wait_after_message(void)
{
    while (getkey() != 0) host_pump();                    /* PORT: busy loop pumps the host */
    delay_ticks(40);
    if (DSW(DS_demo_mode) == 0) {
        kbd_flush();
        getkey_timeout(1800);
    }
}
