/* game_flow segment 0267: run_game (stage loop), stage results, gas station, record book, duel and
 * section pages, police ending, difficulty screen — port/spec/game_flow.md §4.10-4.14. */
#include <stdio.h>
#include <string.h>

#include "flow.h"
#include "../codeptr.h"
#include "../host.h"
#include "../platform/gfx.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/sound.h"
#include "../platform/timer.h"

/* "%d:%02d.%d" parts of a 0.1 s time (16-bit IDIV after CWD). */
static void time_parts(s16 t, s16 *m, s16 *s, s16 *d)
{
    s16 rem = flow_imod(t, 600);
    *m = flow_idiv(t, 600);
    *s = flow_idiv(rem, 10);
    *d = flow_imod(rem, 10);
}

/* 0267:0004 plural_s — game_flow.md §2a (verified): "" (DS:7738) or "s" (DS:7739) */
u16 plural_s(s16 n)
{
    return (n == 1) ? 0x7738 : 0x7739;
}

/* 0267:0017 results_line — game_flow.md §4.12 (verified) */
void results_line(const char *s, s16 x)
{
    if (s[0] == 0) return;
    gfx_draw_text_str(s, (u16)x, DSW(DS_results_y));
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 8);
}

static void results_line_ds(u16 s_ds, s16 x)
{
    results_line(DSTR(s_ds), x);
}

/* 0267:118b results_overall — game_flow.md §4.12 (verified) */
void results_overall(void)
{
    char buf[80];
    s16 m, s, d;
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 0x10);
    results_line_ds(0x7C96, 0x48);                                   /* " Overall Performance " */
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 8);
    time_parts(DSS(DS_total_time), &m, &s, &d);
    snprintf(buf, sizeof buf, "Your time:      %d:%02d.%d", m, s, d);                  /* DS:7CAC */
    results_line(buf, 8);
    snprintf(buf, sizeof buf, "Your score:     %ld points", (long)DSSL(DS_total_score)); /* DS:7CC7 */
    results_line(buf, 8);
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 8);
}

/* 0267:1224 results_section_page — game_flow.md §4.12 (verified; the caller's stack arguments are unused) */
void results_section_page(void)
{
    char buf[84];
    s16 m, s, d;
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    DSW(DS_results_y) = 0x20;
    draw_rect_outline(0, 0x24, 0x137, 0x6C, 4);
    draw_rect_outline(0, 0x7C, 0x137, 0xBC, 4);
    snprintf(buf, sizeof buf, " Section %d ", DSS(DS_stage));                            /* DS:7CE2 */
    results_line(buf, 0x40);
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) - 8);
    snprintf(buf, sizeof buf, " %d.%d miles ", flow_idiv(DSS(DS_stage_length), 0x1A4),
             flow_imod(flow_idiv(DSS(DS_stage_length), 0x2A), 10));                      /* DS:7CEF */
    results_line(buf, 0xB0);
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 8);
    time_parts(DSS(DS_stage_time), &m, &s, &d);
    s16 pen = DSS(DS_penalties);
    snprintf(buf, sizeof buf, "Your time:      %d:%02d.%d + %d:%02d penalty", m, s, d,
             flow_idiv(pen, 3), flow_imod((s16)(20 * pen), 60));                         /* DS:7CFD */
    results_line(buf, 8);
    snprintf(buf, sizeof buf, "Your avg speed: %ld mph", (long)DSSL(DS_avg_speed));      /* DS:7D2A */
    results_line(buf, 8);
    snprintf(buf, sizeof buf, "Your score:     %ld points", (long)DSSL(DS_stage_score)); /* DS:7D42 */
    results_line(buf, 8);
}

/* Gas-station picture on the page (0267:00e3-01a0 and 0267:0d4b-0e08). */
static void gas_station_picture(FarPtr gs, FarPtr st, u16 gast_ds, u16 carm_ds, u16 cars_ds)
{
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    blit_copy_own(res_find(gs, gast_ds));                                      /* "gast" */
    blit_copy_own(res_find(gs, DSW(DS_gas_attendant - 2 + 2 * DSS(DS_stage))));   /* DS:02E4 + 2*stage */
    blit_and_clip_hot(res_find(st, carm_ds), 0xA0, 0xA5);                     /* "carM" */
    blit_or_clip_hot(res_find(st, cars_ds), 0xA0, 0xA5);                      /* "carS" */
    mem_release_cache_old(gs);
    mem_release_cache_old(st);
}

/* Police ending — inline in stage_results (0267:0cc0-117e), game_flow.md §4.14 (verified).
 * Returns non-zero when a load failed (stage_results then returns 0x1B). */
static s16 police_ending(void)
{
    char name[26];
    FarPtr xcop[2], guy[14], cop[18];

    music_play(ds_far(DS_songs), 3);
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x7B66)),
                      DSS(DS_car_disk), 0) != 0)
        return 1;                                                              /* "st" */
    FarPtr st = load_shapes(DS_disk_path);
    if (ensure_disk(0x7B69, DSS(DS_data_disk), 0) != 0) {                      /* "gasstuff" */
        mem_release_cache_old(st);
        return 1;
    }
    FarPtr gs = load_shapes(DS_disk_path);
    gas_station_picture(gs, st, 0x7B72, 0x7B77, 0x7B7C);
    if (ensure_disk(0x7B81, DSS(DS_data_disk), 0) != 0) return 1;             /* "endgame" */
    FarPtr e = load_shapes(DS_disk_path);
    FarPtr work = gfx_create_buffer(0x140, 0xC8, 0x0F);                        /* mask accumulator */
    FarPtr bg = gfx_create_buffer(0x140, 0xC8, 0x0F);                          /* gas station */
    gfx_select_target(bg);
    blit_copy_own(flow_page_sprite());
    gfx_select_target(work);
    gfx_clear_clip(0x0F);
    flow_find_list(e, DSTR(0x7B89), xcop, 2);       /* "xcopcopM" */
    flow_find_list(e, DSTR(0x7B92), guy, 14);       /* "guy0gu0M...guy6gu6M" */
    flow_find_list(e, DSTR(0x7BCB), cop, 18);       /* "cop0co0M...copCcoCM" */
    blit_and_hot(xcop[1], 0xB0, 0xAF);
    for (s16 i = 0; i < 19; i++) {
        s16 g = DSS(DS_end_guy_frame2 + 2 * i);
        s16 c = DSS(DS_end_cop_frame2 + 2 * i);
        if (i == 7) music_play(ds_far(DS_songs), 2);
        deadline_set((u32)(s32)DSS(DS_end_frame_ticks + 2 * i));
        gfx_select_target(work);
        blit_copy_hot(guy[g + 1], 0xB0, 0xAF);      /* masks accumulate on the white page */
        blit_copy_hot(cop[c + 1], 0xB0, 0xAF);
        gfx_select_target(flow_page_desc());
        blit_copy_own(gfx_desc_sprite(work));
        blit_and_own(gfx_desc_sprite(bg));           /* background through the mask */
        blit_or_hot(xcop[0], 0xB0, 0xAF);
        blit_or_hot(guy[g], 0xB0, 0xAF);
        blit_or_hot(cop[c], 0xB0, 0xAF);
        if (i == 0) {
            screen_reveal(2);
        } else {
            select_screen();
            blit_copy_own(flow_page_sprite());
        }
        deadline_wait();
    }
    delay_ticks(700);
    gfx_fill_rect(10, 10, 300, 0x46, 0);                                       /* on the screen */
    draw_rect_outline(11, 11, 0x134, 0x4E, 0xFFFF);
    gfx_set_text_colours(0x0F, 0);
    draw_text_centered(0x7C14, 0x12);               /* "License revoked and a 30" */
    draw_text_centered(0x7C2D, 0x1A);               /* "day jail sentence for the" */
    draw_text_centered(0x7C47, 0x22);               /* "following infractions:" */
    draw_text_centered(0x7C5E, 0x30);               /* "Excessive speed" */
    draw_text_centered(0x7C6E, 0x38);               /* "Reckless driving" */
    draw_text_centered(0x7C7F, 0x40);               /* "Evading highway patrol" */
    delay_ticks(1200);
    music_play(ds_far(DS_songs), 1);
    kbd_flush();
    gfx_free_buffer(bg);
    gfx_free_buffer(work);
    mem_release_cache_old(e);
    return 0;
}

/* 0267:0039 stage_results — game_flow.md §4.12 (verified against the disassembly).
 * kind: 0 = normal stage (gas station), 1 = last stage, 99 = game over. */
s16 stage_results(s16 kind)
{
    char buf[96], name[26];
    s16 m, s, d;
    s16 key;

    DSS(DS_penalties) = (s16)(DSS(DS_crashes) + DSS(DS_engines_blown) + DSS(DS_out_of_gas));

    if (kind == 0) {                                                 /* ---- gas station ---- */
        DSS(DS_lives)++;
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x773B)),
                          DSS(DS_car_disk), 0) != 0)
            return KEY_ESC;                                          /* "st" */
        FarPtr st = load_shapes(DS_disk_path);
        if (ensure_disk(0x773E, DSS(DS_data_disk), 0) != 0) {       /* "gasstuff" */
            mem_release_cache_old(st);
            return KEY_ESC;
        }
        FarPtr gs = load_shapes(DS_disk_path);
        gas_station_picture(gs, st, 0x7747, 0x774C, 0x7751);
        screen_reveal(2);
        FarPtr text = gfx_create_buffer(0x140, 0x38, 0x0F);
        kbd_flush();
        if (getkey_timeout(200) == KEY_ESC) return KEY_ESC;          /* the text buffer leaks (original) */
        gfx_select_target(text);
        gfx_clear_clip(0);
        gfx_set_text_colours(0x0F, 0);
        DSW(DS_results_y) = 0;
        if (DSS(DS_crashes) != 0) {
            snprintf(buf, sizeof buf, "You crashed %d time%s.", DSS(DS_crashes),
                     DSTR(plural_s(DSS(DS_crashes))));                               /* DS:7756 */
            results_line(buf, 0);
        }
        if (DSS(DS_engines_blown) != 0) {
            snprintf(buf, sizeof buf, "You blew your engine %d time%s.", DSS(DS_engines_blown),
                     DSTR(plural_s(DSS(DS_engines_blown))));                         /* DS:776D */
            results_line(buf, 0);
        }
        if (DSS(DS_tickets) != 0) {
            snprintf(buf, sizeof buf, "You were given %d speeding ticket%s.", DSS(DS_tickets),
                     DSTR(plural_s(DSS(DS_tickets))));                               /* DS:778D */
            results_line(buf, 0);
        }
        if (DSS(DS_out_of_gas) != 0) {
            snprintf(buf, sizeof buf, "%s", DSTR(0x77B2));           /* sprintf(buf, "You ran out of gas once.") */
            results_line(buf, 0);
        }
        if ((s16)(DSS(DS_penalties) + DSS(DS_tickets)) == 0) results_line_ds(0x77CB, 0);  /* "A clean run!" */
        snprintf(buf, sizeof buf, "You have %d lives left.", DSS(DS_lives));             /* DS:77D8 */
        results_line(buf, 0);
        results_line_ds(0x77F0, 0);                                  /* "Press key or joystick to continue..." */
        select_screen();
        gfx_set_clip_current(0, 0x28, 0xB0, 0xC8);
        gfx_clear_clip(0);
        s16 y = 0xC8;
        kbd_flush();
        for (;;) {                                                   /* endless scroll in rows 176..199 */
            blit_copy_clip_raw(gfx_desc_sprite(text), 0, y);
            blit_copy_clip_raw(gfx_desc_sprite(text), 0, (s16)(y + 0x38));
            delay_ticks(flow_imod(y, 8) == 0 ? 0x19 : 9);
            y--;
            if (y < 0x80) y = 0xB8;
            key = (s16)getkey();
            if (key == KEY_ESC) return key;
            if (key != 0) break;
        }
        gfx_free_buffer(text);
    }

    /* ---- player stage figures (0267:03f9) ---- */
    s32 T = (s32)DSS(DS_penalties) * 200 + (s32)DSS(DS_stage_time);
    DSSL(DS_avg_speed) = flow_aldiv(flow_aldiv((s32)DSS(DS_player_dist) * 3600, 42), T);
    {
        s32 v = (s32)DSS(DS_player_dist) * DSSL(DS_avg_speed);
        v = v * DSSL(DS_avg_speed);
        v = flow_aldiv(v, (s32)DSS(DS_stage_length));
        v = v * (s32)DSS(DS_stage_score_k);
        DSSL(DS_stage_score) = flow_aldiv(v, 10);
    }
    if (DSS(DS_player_dist) < 100) DSS(DS_player_dist) = 200;       /* after use (sic) */
    DSSL(DS_stage_score) = flow_aldiv((s32)DSS(DS_diff_score_pct) * DSSL(DS_stage_score), 100);
    if (kind == 99) T = flow_aldiv(T * (s32)DSS(DS_stage_length), (s32)DSS(DS_player_dist));
    DSSL(DS_total_score) = (s32)((u32)DSSL(DS_total_score) + (u32)DSSL(DS_stage_score));
    DSW(DS_total_time) = (u16)(DSW(DS_total_time) + (u16)T);
    DSW(DS_stage_time) = (u16)((u16)T - (u16)(s16)(200 * DSS(DS_penalties)));

    /* ---- record book (0267:051e) ---- */
    s16 si = (s16)(DSS(DS_stage) - 1);
    u16 bt = (u16)(DS_best_time + 2 * si);
    u16 ba = (u16)(DS_best_avg + 4 * si);
    u16 bs = (u16)(DS_best_score + 4 * si);
    u16 bct = (u16)(DS_best_cum_time + 4 * si);
    u16 bcs = (u16)(DS_best_cum_score + 4 * si);
    s16 rec = 0, cum = 0;
    if (DSS(bt) == 0 || (s32)DSS(bt) > T) {
        DSW(bt) = (u16)T;
        DSSL(ba) = DSSL(DS_avg_speed);
        rec = 1;
    }
    if (DSSL(bs) < DSSL(DS_stage_score)) {
        DSSL(bs) = DSSL(DS_stage_score);
        rec |= 4;
    }
    if (DSSL(bct) == 0 || (s32)DSS(DS_total_time) < DSSL(bct)) {
        DSSL(bct) = (s32)DSS(DS_total_time);
        cum = 1;
    }
    if (DSSL(bcs) < DSSL(DS_total_score)) {
        DSSL(bcs) = DSSL(DS_total_score);
        cum = 4;
    }
    s32 show_cum_time = DSSL(bct);
    s32 show_cum_score = DSSL(bcs);
    if (cum != 0 && rec == 0) hisc_save();
    if (rec != 0) {
        hisc_save();
        select_screen();
        gfx_clear_clip(0);
        draw_text_centered(0x7815, 0x32);                           /* "Congratulations!!!" */
        draw_text_centered(0x7828, 0x3A);                           /* "on making the record books" */
        if (rec & 1) {
            draw_text_centered(0x7843, 0x5A);                       /* "You beat the fastest time for this" */
            draw_text_centered(0x7866, 0x62);                       /* "road section and had the highest" */
            draw_text_centered(0x7887, 0x6A);                       /* "average speed." */
            if (rec & 4) {
                draw_text_centered(0x7896, 0x82);                   /* "That gives you the highest score for" */
                draw_text_centered(0x78BB, 0x8A);                   /* "this road section." */
            }
        } else {
            draw_text_centered(0x78CE, 0x82);                       /* "You got the highest score for this" */
            draw_text_centered(0x78F1, 0x8A);                       /* "road section." */
        }
        getkey_wait();
    }

    /* ---- duel page (0267:070a) ---- */
    if (DSS(DS_game_mode) != 0) {
        results_section_page();
        s32 T2 = (s32)DSS(DS_opp_penalties) * 200 + (s32)DSS(DS_opp_time);
        T2 = flow_aldiv(T2 * (s32)DSS(DS_stage_length), (s32)DSS(DS_opp_dist));   /* always extrapolated */
        DSSL(DS_opp_avg_speed) = flow_aldiv(flow_aldiv((s32)DSS(DS_stage_length) * 3600, 42), T2);
        {
            s32 v = (s32)DSS(DS_opp_dist) * DSSL(DS_opp_avg_speed);
            v = v * DSSL(DS_opp_avg_speed);
            v = flow_aldiv(v, (s32)DSS(DS_stage_length));
            v = v * (s32)DSS(DS_stage_score_k);
            DSSL(DS_opp_stage_score) = flow_aldiv(v, 10);
        }
        DSSL(DS_opp_stage_score) = flow_aldiv((s32)DSS(DS_diff_score_pct) * DSSL(DS_opp_stage_score), 100);
        DSW(DS_opp_total_time) = (u16)(DSW(DS_opp_total_time) + (u16)T2);
        DSSL(DS_opp_total_score) = (s32)((u32)DSSL(DS_opp_total_score) + (u32)DSSL(DS_opp_stage_score));
        DSW(DS_opp_time) = (u16)((u16)T2 - (u16)(s16)(200 * DSS(DS_opp_penalties)));

        u16 msg;
        if (DSS(DS_lives) == 0)                                         msg = 0x7932;   /* "You are dead!!!" */
        else if (DSSL(DS_opp_stage_score) > DSSL(DS_stage_score))       msg = 0x78FF;   /* "The computer won this round." */
        else                                                            msg = 0x791C;   /* "You won this round!!!" */
        draw_text_centered(msg, 0);
        if (DSS(DS_last_stage) != 0 || DSS(DS_lives) == 0) {
            if (DSSL(DS_opp_total_score) > DSSL(DS_total_score))
                msg = DSS(DS_lives) ? 0x7942 : 0x7964;  /* "Sorry, the computer won the game." / "And you lost the game." */
            else
                msg = DSS(DS_lives) ? 0x797B : 0x79A0;  /* "Congratulations, you won the game!!!" / "However, you still won the game!" */
        } else {
            msg = (DSSL(DS_opp_total_score) > DSSL(DS_total_score))
                ? 0x79C1                                /* "The computer is winning the game." */
                : 0x79E3;                               /* "You are winning the game!!!" */
        }
        draw_text_centered(msg, 0x10);
        DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 8);
        s16 op = DSS(DS_opp_penalties);
        time_parts(DSS(DS_opp_time), &m, &s, &d);
        snprintf(buf, sizeof buf, "Other's time:   %d:%02d.%d + %d:%02d penalty", m, s, d,
                 flow_idiv(op, 3), flow_imod((s16)(20 * op), 60));                        /* DS:79FF */
        results_line(buf, 8);
        snprintf(buf, sizeof buf, "Average speed:  %ld mph", (long)DSSL(DS_opp_avg_speed));    /* DS:7A2C */
        results_line(buf, 8);
        snprintf(buf, sizeof buf, "Other's score:  %ld points", (long)DSSL(DS_opp_stage_score)); /* DS:7A44 */
        results_line(buf, 8);
        results_overall();
        time_parts(DSS(DS_opp_total_time), &m, &s, &d);
        snprintf(buf, sizeof buf, "Other's time:   %d:%02d.%d", m, s, d);                     /* DS:7A5F */
        results_line(buf, 8);
        snprintf(buf, sizeof buf, "Other's score:  %ld points", (long)DSSL(DS_opp_total_score)); /* DS:7A7A */
        results_line(buf, 8);
        DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 0x10);
        results_line_ds(0x7A95, 0);                   /* "Press key or joystick to continue..." */
        screen_reveal(2);
        kbd_flush();
        if (getkey_wait() == KEY_ESC) return KEY_ESC;
    }

    /* ---- section page (0267:0a50) ---- */
    results_section_page();
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 8);
    s16 variant = (s16)rand8();
    variant = (variant < 0x55) ? 0 : (variant < 0xAB) ? 1 : 2;
    s32 avg = DSSL(DS_avg_speed);
    s16 rating = ((s32)DSS(DS_rating_speed1) < avg) ? 0
               : ((s32)DSS(DS_rating_speed2) < avg) ? 1
               : ((s32)DSS(DS_rating_speed3) < avg) ? 2 : 3;
    /* DS:02FA is indexed in bytes: 12 per rating (3 variants x 2 lines), 4 per variant */
    u16 moff = (u16)(12 * rating + 4 * variant);
    draw_text_centered(DSW(DS_rating_msgs + moff), 0);
    draw_text_centered(DSW(DS_rating_msgs + moff + 2), 8);
    time_parts(DSS(bt), &m, &s, &d);
    snprintf(buf, sizeof buf, "Best time:      %d:%02d.%d", m, s, d);                        /* DS:7ABA */
    results_line(buf, 8);
    snprintf(buf, sizeof buf, "Best avg speed: %ld mph", (long)DSSL(ba));                    /* DS:7AD5 */
    results_line(buf, 8);
    snprintf(buf, sizeof buf, "Best score:     %ld points", (long)DSSL(bs));                 /* DS:7AED */
    results_line(buf, 8);
    results_overall();
    snprintf(buf, sizeof buf, "Best time:      %ld:%02ld.%ld",                               /* DS:7B08 */
             (long)flow_aldiv(show_cum_time, 600),
             (long)flow_aldiv(flow_alrem(show_cum_time, 600), 10),
             (long)flow_alrem(flow_alrem(show_cum_time, 600), 10));
    results_line(buf, 8);
    snprintf(buf, sizeof buf, "Best score:     %ld points", (long)show_cum_score);           /* DS:7B26 */
    results_line(buf, 8);
    DSW(DS_results_y) = (u16)(DSW(DS_results_y) + 0x10);
    results_line_ds(0x7B41, 0);                       /* "Press key or joystick to continue..." */
    screen_reveal(2);
    kbd_flush();
    key = (s16)getkey_wait();

    if (kind == 1 && DSW(DS_outran_police) != 0) {    /* ---- police ending (0267:0cc0) ---- */
        if (police_ending() != 0) return KEY_ESC;
        key = 0;
    }
    kbd_flush();
    return key;
}

/* 0267:139b difficulty_screen — game_flow.md §4.10 (verified) */
s16 difficulty_screen(void)
{
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    FarPtr a = load_shapes(0x7D5D);                   /* "gamediff" */
    blit_copy_own(res_find(a, 0x7D66));               /* "tex0" */
    blit_copy_own(res_find(a, 0x7D6B));               /* "tex1" */
    blit_copy_own(res_find(a, 0x7D70));               /* "scal" */
    FarPtr sca2 = res_find(a, 0x7D75);                /* "sca2" */
    FarPtr arrw = res_find(a, 0x7D7A);                /* "arrw" */
    FarPtr ARRW = res_find(a, 0x7D7F);                /* "ARRW" (mask) */
    s16 prev = (s16)(DSS(DS_difficulty) + 1);
    screen_reveal(2);
    kbd_flush();
    for (;;) {
        if (prev != DSS(DS_difficulty)) {
            s16 x = (s16)(DSS(DS_difficulty) * 15 + 0x42);
            blit_copy_own(sca2);
            blit_and_raw(ARRW, x, 0x36);
            blit_or_raw(arrw, x, 0x36);
            prev = DSS(DS_difficulty);
        }
        u16 k = getkey_timeout(DSW(DS_demo_mode) ? 100 : 3000);
        if (k == 0) {
            k = menu_random_key();
            if (k == KEY_ESC) k = 0;
            DSW(DS_demo_mode) = 1;
        } else {
            DSW(DS_demo_mode) = 0;
        }
        if (k == KEY_ESC) {
            mem_release_cache(a);
            return KEY_ESC;
        }
        if (k == KEY_ENTER || k == KEY_SPACE) {
            s16 dv = DSS(DS_difficulty);
            DSS(DS_diff_score_pct) = (s16)(flow_idiv((s16)(dv * 0x43), 11) + 0x21);   /* 33..100 */
            DSS(DS_diff_b) = (s16)(flow_idiv((s16)(dv << 7), 11) + 0x7F);             /* 127..255 */
            DSS(DS_diff_c) = (s16)(flow_idiv((s16)(dv * 0x5A), 11) + 0x5A);           /* 90..180 */
            DSS(DS_diff_easy) = (dv < 4 && DSW(DS_demo_mode) == 0) ? 1 : 0;
            mem_release_cache(a);
            return 0;
        }
        if ((k == KEY_RIGHT || k == KEY_DOWN) && DSS(DS_difficulty) < 11) DSS(DS_difficulty)++;
        if ((k == KEY_LEFT || k == KEY_UP) && DSS(DS_difficulty) > 0) DSS(DS_difficulty)--;
    }
}

/* Unpacked size (u24 at +1 of the packed header, FORMATS.md "Packed files"); 0 if unreadable. */
static u32 packed_file_size(const char *name)
{
    u32 size = 0;
    char *path = host_game_path(name, false);
    FILE *fp = path ? fopen(path, "rb") : NULL;
    host_free(path);
    if (fp) {
        unsigned char h[4];
        if (fread(h, 1, 4, fp) == 4) size = h[1] | (u32)h[2] << 8 | (u32)h[3] << 16;
        fclose(fp);
    }
    return size;
}

/* Handles released after a stage, in the original's order (0267:16e0). */
static const u16 stage_handles[] = {
    DS_scn_font, DS_scn_signs, DS_scenery_arc, DS_scn_car3_arc, DS_scn_car2_arc, DS_scn_car1_arc,
    DS_opp_road_arc, DS_dash_arc, DS_cop_arc, DS_road_arc,
};

/* Loads one stage's files (0267:16c1-1c2e). Returns false when a load failed (never in the port). */
static bool run_game_load_stage(void)
{
    char name[26];
    FarPtr h;

    if (ensure_disk(0x7DA1, DSS(DS_data_disk), 0) != 0) return false;                  /* "road" */
    ds_far_wr(DS_road_arc, load_shapes(DS_disk_path));
    if (ensure_disk(0x7DA6, DSS(DS_data_disk), 0) != 0) return false;                  /* "cop" */
    ds_far_wr(DS_cop_arc, load_shapes(DS_disk_path));
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x7DAA)),
                      DSS(DS_car_disk), 0) != 0)
        return false;                                                                    /* "dash" */
    ds_far_wr(DS_dash_arc, load_shapes(DS_disk_path));
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x7DAF)),
                      DSS(DS_car_disk), 0) != 0)
        return false;                                                                    /* ".bin" */
    h = load_raw(DS_disk_path);
    far_memcpy(h, ds_ptr(0x23A6), 0x34F);                                                /* car .BIN */
    mem_release_cache(h);
    if (DSS(DS_game_mode) != 0) {
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_opp_car_code), DSTR(0x7DB4)),
                          DSS(DS_opp_disk), 0) != 0)
            return false;                                                                /* "road" */
        ds_far_wr(DS_opp_road_arc, load_shapes(DS_disk_path));
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_opp_car_code), DSTR(0x7DB9)),
                          DSS(DS_opp_disk), 0) != 0)
            return false;                                                                /* "o.bin" */
        h = load_raw(DS_disk_path);
        far_memcpy(h, ds_ptr(DS_opp_bin), 0x20);
        mem_release_cache(h);
    }
    if (DSW(DS_demo_mode) != 0) {
        DSS(DS_lives) = 1;
        /* stage = sign-preserving (|rand8 * (stages - 1)| >> 8), 16-bit IMUL (0267:19d8-1a00) */
        s16 v = (s16)(u16)((u16)rand8() * (u16)(scn_stages(DSS(DS_scn_idx)) - 1));
        s16 sg = (s16)(v >> 15);
        u16 mag = (u16)((v ^ sg) - sg);
        s16 q = (s16)((s16)mag >> 8);
        DSS(DS_stage) = (s16)((q ^ sg) - sg);
    }
    /* sprintf(name, "%s%c%s", scn_code, '0' + stage, ".dat") */
    snprintf(name, sizeof name, "%s%c%s", DSTR(DS_scn_code), (char)('0' + DSS(DS_stage)), DSTR(0x7DBF));
    if (ensure_disk_c(name, DSS(DS_scn_disk), 0) != 0) return false;
    {
        char path[26];
        snprintf(path, sizeof path, "%s", DSTR(DS_disk_path));
        u32 size = packed_file_size(path);
        h = unpack_file(DS_disk_path);
        /* PORT: the original copies a fixed 0x1E5E bytes; bytes past the decoded data are zero (§10.3). */
        u16 n = (size != 0 && size < 0x1E5E) ? (u16)size : 0x1E5E;
        far_memcpy(h, ds_ptr(DS_stage_dat), n);
        memset(mp(DGROUP, (u16)(DS_stage_dat + n)), 0, (size_t)(0x1E5E - n));
        mem_release_cache(h);
    }
    static const u16 car_names[3] = { 0x7DCB, 0x7DD0, 0x7DD5 };                        /* "car1".."car3" */
    static const u16 car_arcs[3] = { DS_scn_car1_arc, DS_scn_car2_arc, DS_scn_car3_arc };
    for (int i = 0; i < 3; i++) {
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_scn_code), DSTR(car_names[i])),
                          DSS(DS_scn_disk), 0) != 0)
            return false;
        ds_far_wr(car_arcs[i], load_shapes(DS_disk_path));
    }
    if (ensure_disk(DS_stage_dat, DSS(DS_scn_disk), 0) != 0) return false;             /* name in the .DAT */
    ds_far_wr(DS_scenery_arc, load_shapes(DS_disk_path));
    snprintf(name, sizeof name, "%s%c%s", DSTR(DS_scn_code), (char)('0' + DSS(DS_stage)), DSTR(0x7DDA));  /* ".sgn" */
    if (ensure_disk_c(name, DSS(DS_scn_disk), 0) != 0) return false;
    if (mem_is_cached_c(name) != 0 || find_first(DS_disk_path) != 0) {                 /* optional */
        ds_far_wr(DS_scn_signs, load_raw(DS_disk_path));
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_scn_code), DSTR(0x7DE6)),
                          DSS(DS_scn_disk), 0) != 0)
            return false;                                                                /* ".fnt" */
        ds_far_wr(DS_scn_font, load_raw(DS_disk_path));
    }
    return true;
}

/* 0267:15e4 run_game — game_flow.md §1.3, §4.11 (verified against the disassembly).
 * Returns 0 to go back to the menu, 1 when a game (or an attract-mode stage) ended. */
s16 run_game(s16 mode)
{
    DSSL(DS_total_score) = 0;
    DSSL(DS_opp_total_score) = 0;
    DSW(DS_total_time) = 0;
    DSW(DS_opp_total_time) = 0;
    DSW(DS_outran_police) = 0;
    DSS(DS_game_mode) = mode;
    if (difficulty_screen() != 0) return 0;
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    draw_text_centered(0x7D84, 0x60);                 /* "Please wait while loading..." */
    screen_reveal(2);
    DSS(DS_stage) = 0;
    DSS(DS_lives) = 5;
    for (;;) {
        s16 r;
        for (size_t i = 0; i < sizeof stage_handles / sizeof stage_handles[0]; i++)
            ds_far_wr(stage_handles[i], far_make(0, 0));
        gfx_free_buffer(flow_page_desc());
        DSS(DS_last_stage) = (scn_stages(DSS(DS_scn_idx)) == DSS(DS_stage) + 1) ? 1 : 0;
        if (run_game_load_stage()) {
            r = run_stage();                          /* 06c9:1b2c */
            timer_install_div((s16)PIT_DIV_GAME);     /* 06c9:6059(0x2E9C) */
            timer_add_routine(codeptr_far(FN_music_tick));
        } else {
            r = -1;
        }
        for (size_t i = 0; i < sizeof stage_handles / sizeof stage_handles[0]; i++) {
            FarPtr p = ds_far(stage_handles[i]);
            if (!far_is_null(p)) mem_release_cache(p);
        }
        ds_far_wr(DS_page_buf_desc, gfx_create_buffer(0x140, 0xC8, 0x0F));
        music_play(ds_far(DS_songs), 1);
        if (r == -1) return 0;
        if (DSW(DS_demo_mode) != 0) return 1;
        DSS(DS_stage)++;
        if (DSS(DS_lives) == 0) {
            stage_results(99);
            hisc_check();
            return 1;
        }
        if (DSS(DS_last_stage) != 0) {
            if (stage_results(1) == KEY_ESC) return 0;
            hisc_check();
            return 1;
        }
        if (stage_results(0) == KEY_ESC) return 0;
    }
}
