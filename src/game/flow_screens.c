/* game_flow screens: intro (00c0, 010c), showroom (0143), main menu, car and scenery selection (019e) —
 * port/spec/game_flow.md §4.5-4.9. */
#include <stdio.h>
#include <string.h>

#include "flow.h"
#include "../codeptr.h"
#include "../host.h"
#include "../platform/gfx.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/timer.h"

#define LOGO_LINES 38               /* DS:007E / DS:01AE: 38 x {x0, y0, x1, y1} */

/* (a * s + b * (32 - s)) >> 5 with 16-bit IMUL products, 16-bit sum and an arithmetic shift. */
static s16 morph(s16 a, s16 s, s16 b, s16 t)
{
    u16 v = (u16)((u16)(s16)(a * s) + (u16)(s16)(b * t));
    return (s16)((s16)v >> 5);
}

/* ================================================================================================
 * Segment 00c0: title, Accolade, intro sequence
 * ============================================================================================== */

/* 00c0:0004 title_screen — game_flow.md §4.5 (verified against the disassembly) */
s16 title_screen(void)
{
    s16 start[LOGO_LINES][4];
    s16 k;
    for (int i = 0; i < LOGO_LINES; i++) {
        start[i][0] = 0xA0; start[i][1] = 0x64; start[i][2] = 0xA0; start[i][3] = 0x64;
    }
    select_screen();
    gfx_clear_clip(0);
    for (s16 s = 0; s <= 0x20; s++) {
        s16 t = (s16)(0x20 - s);
        gfx_select_target(flow_page_desc());
        gfx_set_clip_current(5, 0x23, 0x5E, 0x65);
        gfx_clear_clip(0);
        for (int i = 0; i < LOGO_LINES; i++) {
            u16 l = (u16)(DS_logo_lines_a + 8 * i);
            gfx_draw_line(morph(DSS(l),     s, start[i][0], t),
                          morph(DSS(l + 2), s, start[i][1], t),
                          morph(DSS(l + 4), s, start[i][2], t),
                          morph(DSS(l + 6), s, start[i][3], t), 0xFF);   /* colour 0xFFFF */
        }
        select_screen();
        gfx_set_clip_current(5, 0x23, 0x5E, 0x65);
        blit_copy_clip_own(flow_page_sprite());
        flow_pace(3);                                /* PORT: unpaced in the original (§7) */
        k = (s16)getkey();
        if (k != 0) return k;
    }
    getkey_timeout(100);                             /* result ignored */
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    FarPtr a = load_shapes(0x71E6);                  /* "testdrv2" */
    blit_copy_own(res_find(a, 0x71EF));              /* "tdri" */
    blit_copy_own(res_find(a, 0x71F4));              /* "duel" */
    mem_release_cache_old(a);
    k = screen_reveal(1);
    if (k != 0) return k;
    return (s16)getkey_timeout(300);
}

/* 00c0:027a accolade_screen — game_flow.md §4.5 (verified) */
s16 accolade_screen(void)
{
    s16 r;
    select_screen();
    gfx_clear_clip(0);
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    FarPtr a = load_shapes(0x71F9);                  /* "accolade" */
    blit_copy_own(res_find(a, 0x7202));              /* "acc_" */
    blit_copy_own(res_find(a, 0x7207));              /* "pres" */
    blit_copy_own(res_find(a, 0x720C));              /* "copy" */
    r = screen_reveal(1);
    if (r == 0) {
        FarPtr bull = res_find(a, 0x7211);           /* "bull" slides in from the left */
        for (s16 x = 0; spr_x(bull) > x; x = (s16)(x + 2)) {
            deadline_set(1);
            blit_copy_clip_raw(bull, x, spr_y(bull));
            r = (s16)getkey_until_deadline();
            if (r != 0) break;
        }
    }
    mem_release_cache_old(a);
    return r;
}

/* 00c0:039c intro_sequence — game_flow.md §4.5 (verified) */
s16 intro_sequence(void)
{
    static const u16 preload[3] = { 0x7216, 0x721F, 0x7228 };   /* "dsititle", "testdrv2", "accolade" */
    s16 r = (s16)getkey();
    if (r != 0) return r;
    for (int i = 0; i < 3; i++) {
        if (ensure_disk(preload[i], DSS(DS_data_disk), 0) != 0) return KEY_ESC;
        mem_release_cache(load_shapes(DS_disk_path));
    }
    if ((r = accolade_screen()) != 0) return r;
    if ((r = (s16)getkey_timeout(300)) != 0) return r;
    if ((r = showroom_intro(100)) != 0) return r;
    if ((r = title_screen()) != 0) return r;
    if ((r = credits()) != 0) return r;
    if (DSS(DS_nscenes) == 0) return r;
    return hisc_show(700);
}

/* ================================================================================================
 * Segment 010c: DSI logo and credits
 * ============================================================================================== */

/* 010c:000e dsi_logo_screen — game_flow.md §4.5 (verified) */
s16 dsi_logo_screen(void)
{
    s16 k;
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    for (s16 s = 0; s <= 0x20; s++) {
        s16 t = (s16)(0x20 - s);
        gfx_select_target(flow_page_desc());
        gfx_set_clip_current(2, 0x23, 0x5E, 0xBD);
        gfx_clear_clip(0);
        deadline_set(5);
        for (int i = 0; i < LOGO_LINES; i++) {
            u16 b = (u16)(DS_logo_lines_b + 8 * i);
            u16 a = (u16)(DS_logo_lines_a + 8 * i);
            gfx_draw_line(morph(DSS(b),     s, DSS(a),     t),
                          morph(DSS(b + 2), s, DSS(a + 2), t),
                          morph(DSS(b + 4), s, DSS(a + 4), t),
                          morph(DSS(b + 6), s, DSS(a + 6), t), 0xFF);    /* colour 0x0000FFFF */
        }
        select_screen();
        gfx_set_clip_current(2, 0x23, 0x5E, 0xBD);
        if (s == 0) {
            gfx_set_clip_current(0, 0x28, 0, 0x65);
            k = screen_reveal(0);
            if (k != 0) return k;
        } else {
            blit_copy_own(flow_page_sprite());
        }
        k = (s16)getkey_until_deadline();
        if (k != 0) return k;
    }
    FarPtr arc = load_shapes(0x7232);                /* "dsititle" */
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    blit_or_own(res_find(arc, 0x723B));              /* "sqar" */
    blit_or_own(res_find(arc, 0x7240));              /* "text" */
    blit_or_own(res_find(arc, 0x7245));              /* "arow" */
    k = screen_reveal(1);
    if (k == 0) blit_or_own(res_find(arc, 0x724A));  /* "type", onto the screen */
    mem_release_cache(arc);
    return k;
}

/* 010c:0285 credits — game_flow.md §4.5 (verified) */
s16 credits(void)
{
    s16 r = dsi_logo_screen();
    if (r != 0) return r;
    gfx_set_text_colours(0x0F, 1);
    draw_text_centered(0x724F, 0x00);                /* " Created by " */
    draw_text_centered(0x725C, 0x20);                /* " Design and Programming " */
    draw_text_centered(0x7275, 0x48);                /* " Art " */
    draw_text_centered(0x727B, 0x60);                /* " Music " */
    gfx_set_text_colours(0x0E, 0);
    draw_text_centered(0x7283, 0x0C);                /* "Distinctive Software Inc." */
    draw_text_centered(0x729D, 0x14);                /* "Vancouver B.C." */
    draw_text_centered(0x72AC, 0x2C);                /* "Amory Wong   Rick Friesen Don Mattrick " */
    draw_text_centered(0x72D4, 0x34);                /* "Bruce Dawson Chris Taylor Al Johanson  " */
    draw_text_centered(0x72FC, 0x3C);                /* "Brad Gour    Erik Kiss    Kris Hatlelid" */
    draw_text_centered(0x7324, 0x54);                /* "John Boechler  Tony Lee  Theresa Henry" */
    draw_text_centered(0x734B, 0x6C);                /* "Kris Hatlelid" */
    return (s16)getkey_timeout(500);
}

/* ================================================================================================
 * Segment 0143: showroom
 * ============================================================================================== */

/* 0143:000a showroom_intro — game_flow.md §4.6 (verified) */
s16 showroom_intro(s16 unused)
{
    char name[20];
    s16 first = 0, second = 1;
    s16 r;
    (void)unused;
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    if (car_disk(1) != DSS(DS_data_disk)) first = 0;
    if (car_disk(second) != DSS(DS_data_disk)) second = (DSS(DS_ncars_main) > 1) ? 1 : 0;
    /* preload the second car so no disk swap happens during the animation */
    ensure_disk_c(flow_concat(name, sizeof name, DSTR(car_rec(second)), DSTR(0x735A)), car_disk(second), 0);  /* "st" */
    mem_release_cache(load_shapes(DS_disk_path));
    ensure_disk_c(flow_concat(name, sizeof name, DSTR(car_rec(second)), DSTR(0x735D)), car_disk(second), 0);  /* ".ss" */
    mem_release_cache(load_raw(DS_disk_path));
    if (ensure_disk(car_rec(first), car_disk(first), 0) != 0) return KEY_ESC;
    r = showroom_drive(DSTR(DS_disk_path), 0xA0, 0xBE, 1, 1);
    if (r != 0) return r;
    if (ensure_disk(car_rec(second), car_disk(second), 0) != 0) return KEY_ESC;
    return showroom_drive(DSTR(DS_disk_path), 0x1E0, 0xBE, 0, 0);   /* enters from x = 480 at full speed */
}

/* 0143:019e showroom_tick — game_flow.md §4.6 (verified); timer routine */
void showroom_tick(void)
{
    DSS(DS_showroom_speed)++;
    DSS(DS_ss_ticks)++;
    if (DSS(DS_showroom_speed) > 0xC8) DSS(DS_showroom_speed) = 0xC8;
    DSS(DS_showroom_dist) = (s16)(DSS(DS_showroom_dist) + flow_idiv(DSS(DS_showroom_speed), 0x14));
    if (flow_imod(DSS(DS_ss_ticks), DSS(DS_ss_frame_ticks)) == 0) {
        DSS(DS_ss_frame)++;
        if (DSS(DS_ss_frame) >= DSS(DS_ss_frames)) DSS(DS_ss_frame) = (s16)(DSS(DS_ss_frames) - 1);
    }
}

/* sscanf over a C buffer (MSC semantics as in flow.c): %d into *out, %s into a C buffer. */
static bool buf_scan_int(FlowFile *f, s16 *out) { return flow_scan_int(f, out); }
static bool buf_scan_cstr(FlowFile *f, char *dst, size_t cap)
{
    while (f->pos < f->len && (f->data[f->pos] == ' ' || (f->data[f->pos] >= 9 && f->data[f->pos] <= 13))) f->pos++;
    if (f->pos >= f->len) return false;
    size_t n = 0;
    while (f->pos < f->len && !(f->data[f->pos] == ' ' || (f->data[f->pos] >= 9 && f->data[f->pos] <= 13))) {
        if (n + 1 < cap) dst[n++] = (char)f->data[f->pos];
        f->pos++;
    }
    dst[n] = 0;
    return true;
}

#define SS_MAX_FRAMES 64            /* PORT: the original's array holds 25 far pointers (.SS files use <= 15) */

/* 0143:01e1 showroom_drive — game_flow.md §4.6 (verified against the disassembly) */
s16 showroom_drive(const char *path, s16 x, s16 y, s16 anim, s16 reveal)
{
    char name[32];
    char buf[257];
    char names[256];
    FarPtr win[SS_MAX_FRAMES];
    FarPtr front[3], rear[3];
    s16 count = 0;
    int nwin = 0;
    s16 r = 0;
    char pathc[32];
    snprintf(pathc, sizeof pathc, "%s", path);       /* path may be DS:8644, which ensure_disk rewrites */

    names[0] = 0;
    if (anim) {
        FarPtr h = load_raw_c(flow_concat(name, sizeof name, pathc, DSTR(0x7361)));   /* ".ss" */
        for (u16 i = 0; i < 0x100; i++) buf[i] = (char)far_mp(far_add(h, i))[0];      /* 06c9:5d01 */
        buf[256] = 0;
        mem_release_cache(h);
        FlowFile f = { (unsigned char *)buf, strlen(buf), 0, false };
        /* sscanf(buf, "%d %d %d %s", &ss_frames, &count, &ss_frame_ticks, names) */
        s16 v;
        if (buf_scan_int(&f, &v)) {
            DSS(DS_ss_frames) = v;
            if (buf_scan_int(&f, &v)) {
                count = v;
                if (buf_scan_int(&f, &v)) {
                    DSS(DS_ss_frame_ticks) = v;
                    buf_scan_cstr(&f, names, sizeof names);
                }
            }
        }
    }
    FarPtr a = load_shapes_c(flow_concat(name, sizeof name, pathc, DSTR(0x7371)));    /* "st" */
    if (anim) nwin = flow_find_list(a, names, win, SS_MAX_FRAMES);
    FarPtr carS = res_find(a, 0x7374);               /* "carS" */
    res_find(a, 0x7379);                             /* "carM" (unused) */
    res_find(a, 0x737E);                             /* "logo" (unused) */
    flow_find_list(a, DSTR(0x7383), front, 3);       /* "frm0frm1frm2" */
    flow_find_list(a, DSTR(0x7390), rear, 3);        /* "rrm0rrm1rrm2" */
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    gfx_set_clip_current(0, 0x28, (s16)(y - 0x50), y);
    if (anim) {
        if (reveal) {
            blit_copy_clip_hot(carS, x, y);
            r = screen_reveal(1);
            if (r != 0) goto out;
        } else {
            select_screen();
            blit_copy_clip_hot(carS, x, y);
        }
        r = (s16)getkey_timeout(0x96);
        if (r != 0) goto out;
        for (DSS(DS_ss_frame) = 0; DSS(DS_ss_frame) < count; DSS(DS_ss_frame)++) {   /* window opens */
            deadline_set((u32)(s32)DSS(DS_ss_frame_ticks));
            s16 fi = DSS(DS_ss_frame);
            if (fi >= 0 && fi < nwin) blit_copy_clip_hot(win[fi], x, y);   /* PORT: bounded */
            r = (s16)getkey_until_deadline();
            if (r != 0) goto out;
        }
    }
    DSS(DS_showroom_dist) = 0;
    DSS(DS_showroom_speed) = (x > 300) ? 0xC8 : 0;
    DSS(DS_ss_ticks) = 0;
    timer_add_routine(codeptr_far(FN_showroom_tick));
    for (;;) {
        if ((s16)(x - DSS(DS_showroom_dist)) <= -200) break;
        s16 d = DSS(DS_showroom_dist);
        s16 w = flow_imod(flow_idiv(d, 12), 3);
        s16 cx = (s16)(x - d);
        gfx_select_target(flow_page_desc());
        gfx_set_clip_current(0, 0x28, (s16)(y - 0x50), y);
        gfx_clear_clip(0);
        blit_copy_clip_hot(carS, cx, y);
        if (anim) {
            s16 fi = DSS(DS_ss_frame);
            if (fi >= 0 && fi < nwin) blit_copy_clip_hot(win[fi], cx, y);  /* PORT: bounded */
        }
        blit_copy_clip_hot(front[w], cx, y);
        blit_copy_clip_hot(rear[w], cx, y);
        select_screen();
        gfx_set_clip_current(0, 0x28, (s16)(y - 0x50), y);
        blit_copy_clip_raw(flow_page_sprite(), 0, 0);
        host_pump();                                 /* PORT: the original redrew at CPU speed */
        r = (s16)getkey();
        if (r != 0) break;
    }
out:
    mem_release_cache(a);
    timer_remove_routine(codeptr_far(FN_showroom_tick));
    return r;
}

/* ================================================================================================
 * Segment 019e: menus
 * ============================================================================================== */

/* 019e:0004 menu_random_key — game_flow.md §4.7 (verified) */
u16 menu_random_key(void)
{
    switch (rand8() & 7) {
    case 0:  return KEY_UP;
    case 1:  return KEY_DOWN;
    case 2:
    case 3:  return KEY_ENTER;
    case 4:  return KEY_RIGHT;
    case 5:  return KEY_LEFT;
    case 6:  return KEY_ESC;
    default: return 0;
    }
}

/* 019e:009b car_slide — game_flow.md §4.8 (verified against the disassembly) */
FarPtr car_slide(s16 car, s16 from, s16 to, FarPtr old)
{
    char name[26];
    s16 y0, delta, row;
    mem_release_cache(old);
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(car_rec(car)), DSTR(0x739E)), car_disk(car), 0) != 0)
        return far_make(0, 0);                       /* "st" */
    FarPtr a = load_shapes(DS_disk_path);
    FarPtr t = load_shapes(0x73A1);                  /* "testdrv2" */
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    blit_copy_own(res_find(t, 0x73AA));              /* "blue" */
    blit_and_clip_hot(res_find(a, 0x73AF), 0xA0, 0x50);   /* "carM" */
    blit_or_clip_hot(res_find(a, 0x73B4), 0xA0, 0x50);    /* "carS" */
    blit_or_clip_own(res_find(a, 0x73B9));                /* "logo" */
    if (from > to) { y0 = 0x57; delta = -0x28; }     /* Up: new rows enter at the top */
    else           { y0 = 0;    delta = 0x28; }      /* Down: new rows enter at the bottom */
    select_screen();
    gfx_set_clip_current(0, 0x28, 0, 0x58);
    do {
        if (from > to) { from--; row = (s16)(from - to); }
        else           { from++; row = (s16)(from - to + 0x57); }
        gfx_scroll_window(0, y0, 0x28, 0x57, delta, flow_page_sprite(), row);
        flow_pace(1);                                /* PORT: unpaced in the original (§7) */
    } while (from != to);
    blit_copy_own(res_find(a, 0x73BE));              /* "stat" */
    mem_release_cache(t);
    return a;
}

/* 019e:027d car_select — game_flow.md §4.8 (verified against the disassembly) */
s16 car_select(s16 car)
{
    char name[26];
    u16 k;
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    if (ensure_disk(0x73C3, DSS(DS_data_disk), 2) != 0) return car;    /* "testdrv2" */
    FarPtr t = load_shapes(DS_disk_path);
    blit_copy_own(res_find(t, 0x73CC));              /* "blue" */
    mem_release_cache(t);
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(car_rec(car)), DSTR(0x73D1)), car_disk(car), 0) != 0)
        return car;                                  /* "st" */
    FarPtr a = load_shapes(DS_disk_path);
    blit_and_clip_hot(res_find(a, 0x73D4), 0xA0, 0x50);   /* "carM" */
    blit_or_clip_hot(res_find(a, 0x73D9), 0xA0, 0x50);    /* "carS" */
    blit_or_clip_own(res_find(a, 0x73DE));                /* "logo" */
    blit_copy_clip_own(res_find(a, 0x73E3));              /* "stat" */
    select_screen();
    blit_copy_own(flow_page_sprite());
    for (;;) {
        k = getkey_timeout(3000);
        if (k == 0) k = KEY_ESC;
        if (k == KEY_ESC || k == KEY_ENTER || k == KEY_SPACE) break;
        if (k == KEY_UP) {
            car--;
            if (car < 0) car = (s16)(DSS(DS_ncars) - 1);
            a = car_slide(car, 0, -0x58, a);
        }
        if (k == KEY_DOWN) {
            car++;
            if (car > DSS(DS_ncars) - 1) car = 0;
            a = car_slide(car, -0x58, 0, a);
        }
    }
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    if (!far_is_null(a)) {
        blit_copy_clip_hot(res_find(a, 0x73E8), 0xA0, 0x50);   /* "carS" */
        mem_release_cache_old(a);
    }
    select_screen();
    gfx_set_clip_current(0, 0x28, 0, 0x58);
    blit_copy_clip_hot(flow_page_sprite(), 0, 0);   /* the top 88 rows show only the car on black */
    return car;
}

/* 019e:0527 scenery_select — game_flow.md §4.9 (verified) */
void scenery_select(void)
{
    char buf[48], name[26];
    for (;;) {
        gfx_select_target(flow_page_desc());
        gfx_clear_clip(0);
        if (DSS(DS_nscenes) - 1 < DSS(DS_scn_idx)) DSS(DS_scn_idx) = 0;
        if (DSS(DS_scn_idx) < 0) DSS(DS_scn_idx) = (s16)(DSS(DS_nscenes) - 1);
        strcpy(DSTR(DS_scn_code), DSTR(scn_rec(DSS(DS_scn_idx))));
        DSS(DS_scn_disk) = scn_disk(DSS(DS_scn_idx));
        if (DSS(DS_nscenes) == 1) {
            draw_text_centered(0x73ED, 0);           /* "Additional Scenerydisks Available." */
        } else {
            /* sprintf(buf, "%s DISK", disk_type_names[scn_disk]) */
            snprintf(buf, sizeof buf, "%s DISK", DSTR(DSW(DS_disk_type_names + 2 * DSS(DS_scn_disk))));
            draw_text_centered_str(buf, 0);
        }
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(scn_rec(DSS(DS_scn_idx))), DSTR(0x7418)),
                          DSS(DS_scn_disk), 0) != 0)
            return;                                  /* "icon" */
        FarPtr a = load_shapes(DS_disk_path);
        blit_copy_own(res_find(a, 0x741D));          /* "picl" */
        mem_release_cache(a);
        draw_text_centered(0x7422, 0xBE);            /* "Use Keyboard/Joystick to change scenery." */
        screen_reveal(3);
        s16 old;
        do {
            old = DSS(DS_scn_idx);
            u16 k = getkey_timeout(3000);
            if (k == 0) k = KEY_ESC;
            if (k == KEY_ENTER || k == KEY_SPACE || k == 0 || k == KEY_ESC) return;
            if (k == KEY_UP || k == KEY_RIGHT) DSS(DS_scn_idx)++;
            if (k == KEY_DOWN || k == KEY_LEFT) DSS(DS_scn_idx)--;
        } while (DSS(DS_scn_idx) == old);
    }
}

/* Highlight box of the selected GAMEOPT icon (019e:0a2f-0ad3). */
static void menu_draw_highlight(void)
{
    FarPtr s = ds_far((u16)(DS_gameopt_spr + 4 * DSS(DS_menu_sel)));
    s16 x = spr_x(s), y = spr_y(s), w = spr_w(s), h = spr_h(s);
    draw_rect_outline((s16)(x + 2), y, (s16)((s16)(w << 3) + x - 3), (s16)(y + h - 1), 0xFFFF);
    s = ds_far((u16)(DS_gameopt_spr + 4 * DSS(DS_menu_sel)));
    x = spr_x(s); y = spr_y(s); w = spr_w(s); h = spr_h(s);
    draw_rect_outline((s16)(x + 1), (s16)(y - 1), (s16)((s16)(w << 3) + x - 2), (s16)(y + h), 0xFFFF);
}

/* 019e:06c5 main_menu — game_flow.md §4.7 (verified against the disassembly) */
s16 main_menu(void)
{
    char name[26];
    for (;;) {
        FarPtr a, g;
        DSS(DS_menu_prev_sel) = (s16)(DSS(DS_menu_sel) + 1);
        gfx_select_target(flow_page_desc());
        gfx_clear_clip(0);
        gfx_set_text_colours(0x0F, 0);
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x744B)),
                          DSS(DS_car_disk), 0) == 0) {                              /* "rear" */
            a = load_shapes(DS_disk_path);
            blit_copy_clip_hot(res_find(a, 0x7450), 0x28, 0xBF);                     /* "logS" */
            mem_release_cache(a);
        } else {
            gfx_draw_text(0x7455, 0x1C, 0xAA);                                      /* "N/A" */
        }
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_opp_car_code), DSTR(0x7459)),
                          DSS(DS_opp_disk), 0) == 0) {                              /* "rear" */
            a = load_shapes(DS_disk_path);
            blit_copy_clip_hot(res_find(a, 0x745E), 0x78, 0xBF);                     /* "logS" */
            mem_release_cache(a);
        } else {
            gfx_draw_text(0x7463, 0x6C, 0xAA);                                      /* "N/A" */
        }
        if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_scn_code), DSTR(0x7467)),
                          DSS(DS_scn_disk), 0) == 0) {                              /* "icon" */
            a = load_shapes(DS_disk_path);
            blit_copy_own(res_find(a, 0x746C));                                      /* "pics" */
            mem_release_cache(a);
        } else {
            gfx_draw_text(0x7471, 0xBC, 0xAA);                                      /* "N/A" */
        }
        if (ensure_disk(0x7475, DSS(DS_data_disk), 0) != 0) return 1;                /* "gamediff" */
        mem_release_cache(load_shapes(DS_disk_path));                                /* preload */
        if (ensure_disk(0x747E, DSS(DS_data_disk), 0) != 0) return 1;                /* "gameopt" */
        g = load_shapes(DS_disk_path);
        {
            FarPtr spr[9];
            flow_find_list(g, DSTR(0x7486), spr, 9);         /* "cloccompycarocarsceninsttxt0txt1arrw" */
            for (int i = 0; i < 9; i++) ds_far_wr((u16)(DS_gameopt_spr + 4 * i), spr[i]);
        }
        for (int i = 0; i < 9; i++) blit_or_own(ds_far((u16)(DS_gameopt_spr + 4 * i)));
        screen_reveal(3);
        kbd_flush();

        u16 k = 0xFFFF;
        for (;;) {
            if (k == KEY_ESC) {
                mem_release_cache(g);
                return 0;
            }
            if (k == KEY_ENTER || k == KEY_SPACE) break;
            if (k == KEY_LEFT)  DSS(DS_menu_sel)--;
            if (k == KEY_RIGHT) DSS(DS_menu_sel)++;
            if (k == KEY_UP && (DSS(DS_menu_sel) == 2 || DSS(DS_menu_sel) == 3)) DSS(DS_menu_sel) = 0;
            if (k == KEY_UP && (DSS(DS_menu_sel) == 4 || DSS(DS_menu_sel) == 5)) DSS(DS_menu_sel) = 1;
            if (k == KEY_DOWN && DSS(DS_menu_sel) == 0) DSS(DS_menu_sel) = 2;
            if (k == KEY_DOWN && DSS(DS_menu_sel) == 1) DSS(DS_menu_sel) = 5;
            if (DSS(DS_menu_sel) < 0) DSS(DS_menu_sel) = 5;
            if (DSS(DS_menu_sel) > 5) DSS(DS_menu_sel) = 0;
            if (DSS(DS_menu_prev_sel) != DSS(DS_menu_sel)) {
                blit_copy_own(flow_page_sprite());   /* erase the old highlight */
                menu_draw_highlight();
            }
            k = getkey_timeout(DSW(DS_demo_mode) ? 100 : 3000);
            if (k == 0 || k == KEY_F10) {
                DSW(DS_demo_mode) = 1;
                k = menu_random_key();
                if (k == KEY_ESC) k = 0;
            } else {
                DSW(DS_demo_mode) = 0;
            }
            DSS(DS_menu_prev_sel) = DSS(DS_menu_sel);
        }
        mem_release_cache(g);
        if (DSW(DS_demo_mode) != 0) DSS(DS_menu_sel) = 0;
        if (DSB(DS_protect_sabotage) != 0) return 0;

        s16 r = 0;
        switch (DSS(DS_menu_sel)) {
        case 0:
            r = run_game(0);
            break;
        case 1:
            r = run_game(1);
            break;
        case 2:
            DSS(DS_car_idx) = car_select(DSS(DS_car_idx));
            strcpy(DSTR(DS_player_car_code), DSTR(car_rec(DSS(DS_car_idx))));
            DSS(DS_car_disk) = car_disk(DSS(DS_car_idx));
            ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x74AB)),
                          DSS(DS_car_disk), 0);                                      /* ".ss" */
            mem_release_cache(load_raw(DS_disk_path));
            showroom_drive(DSTR(DS_player_car_code), 0xA0, 0x50, 1, 0);
            break;
        case 3:
            DSS(DS_opp_idx) = car_select(DSS(DS_opp_idx));
            strcpy(DSTR(DS_opp_car_code), DSTR(car_rec(DSS(DS_opp_idx))));
            DSS(DS_opp_disk) = car_disk(DSS(DS_opp_idx));
            break;
        case 4:
            scenery_select();
            hisc_load();
            break;
        case 5:
            install_menu();
            if (DSS(DS_ncars) == 0 || DSS(DS_nscenes) == 0) return 1;
            break;
        default:
            break;
        }
        kbd_flush();
        if (DSW(DS_protect_failed) != 0) return 0;
        if (DSW(DS_demo_mode) != 0 && r != 0) return 0;
        DSW(DS_demo_mode) = 0;
    }
}
