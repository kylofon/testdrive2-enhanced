/* game_flow: main (0000:07b3), screen reveal and message boxes, the disk layer replacement and the
 * catalogue files CARS.DAT / SCENES.DAT / select.dat (segment 0432) — port/spec/game_flow.md §4.1-4.4, §9. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow.h"
#include "../codeptr.h"
#include "../host.h"
#include "../platform/gfx.h"
#include "../platform/input.h"
#include "../platform/prompts.h"
#include "../platform/res.h"
#include "../platform/sound.h"
#include "../platform/timer.h"

/* ================================================================================================
 * Port helpers
 * ============================================================================================== */

FarPtr flow_page_sprite(void)
{
    return gfx_desc_sprite(flow_page_desc());
}

s32 flow_aldiv(s32 a, s32 b)
{
    if (b == 0) div_error();
    if (b == -1) return (s32)(0u - (u32)a);          /* _aldiv works on magnitudes: INT32_MIN / -1 wraps */
    return a / b;
}

s32 flow_alrem(s32 a, s32 b)
{
    if (b == 0) div_error();
    if (b == -1) return 0;
    return a % b;
}

s16 flow_idiv(s16 a, s16 b)
{
    return idiv32_16((s32)a, b, NULL);
}

s16 flow_imod(s16 a, s16 b)
{
    s16 r;
    idiv32_16((s32)a, b, &r);
    return r;
}

int flow_stricmp(u16 a_ds, u16 b_ds)
{
    /* 13a8:2b40 stricmp: compares with both characters folded to lower case */
    for (u16 i = 0;; i++) {
        u8 a = DSB(a_ds + i), b = DSB(b_ds + i);
        if (a >= 'A' && a <= 'Z') a = (u8)(a + 0x20);
        if (b >= 'A' && b <= 'Z') b = (u8)(b + 0x20);
        if (a != b) return (int)a - (int)b;
        if (a == 0) return 0;
    }
}

char *flow_concat(char *buf, size_t n, const char *a, const char *b)
{
    snprintf(buf, n, "%s%s", a, b);
    return buf;
}

void flow_pace(u32 ticks)
{
    host_present_now();
    u32 t0 = ticks_get();
    while (ticks_get() - t0 < ticks) host_pump();
}

int flow_find_list(FarPtr arc, const char *names, FarPtr *out, int max)
{
    /* 16eb:000a: for (i = 0; *names; names += 4) out[i++] = res_find(arc, names); */
    int i = 0;
    for (; *names; names += 4) {
        char n4[5];
        int k = 0;
        for (; k < 4 && names[k]; k++) n4[k] = names[k];
        for (int j = k; j < 5; j++) n4[j] = 0;
        if (i >= max) break;                         /* PORT: the original overruns its stack array */
        out[i++] = res_find_c(arc, n4);
        if (k < 4) break;                            /* a short last name ends the string */
    }
    return i;
}

/* ---- MSC text-mode stdio emulation */

bool flow_fopen_read(FlowFile *f, const char *name)
{
    memset(f, 0, sizeof *f);
    char *path = host_game_path(name, false);
    if (!path) return false;
    FILE *fp = fopen(path, "rb");
    host_free(path);
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) size = 0;
    unsigned char *raw = malloc((size_t)size + 1);
    f->data = malloc((size_t)size + 1);
    if (!raw || !f->data) {
        free(raw); free(f->data); f->data = NULL; fclose(fp);
        return false;
    }
    size_t n = fread(raw, 1, (size_t)size, fp);
    fclose(fp);
    for (size_t i = 0; i < n; i++) {                 /* text mode: CR LF -> LF, ^Z ends the file */
        if (raw[i] == 0x1A) break;
        if (raw[i] == '\r' && i + 1 < n && raw[i + 1] == '\n') continue;
        f->data[f->len++] = raw[i];
    }
    free(raw);
    return true;
}

void flow_fclose(FlowFile *f)
{
    free(f->data);
    memset(f, 0, sizeof *f);
}

static int ff_getc(FlowFile *f)
{
    if (f->pos >= f->len) { f->eof = true; return -1; }
    return f->data[f->pos++];
}

static void ff_ungetc(FlowFile *f, int c)
{
    if (c >= 0 && f->pos > 0) f->pos--;
}

static bool ff_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int ff_skip_ws(FlowFile *f)
{
    int c;
    do c = ff_getc(f); while (c >= 0 && ff_isspace(c));
    return c;
}

bool flow_scan_str(FlowFile *f, u16 dst_ds, u16 cap)
{
    int c = ff_skip_ws(f);
    if (c < 0) return false;                         /* input failure: nothing stored */
    u16 n = 0;
    while (c >= 0 && !ff_isspace(c)) {
        if (n + 1u < cap) DSB(dst_ds + n++) = (u8)c; /* PORT: bounded (the original's %s is not) */
        c = ff_getc(f);
    }
    ff_ungetc(f, c);
    DSB(dst_ds + n) = 0;
    return true;
}

bool flow_scan_int(FlowFile *f, s16 *out)
{
    int c = ff_skip_ws(f);
    bool neg = false;
    if (c == '-' || c == '+') { neg = c == '-'; c = ff_getc(f); }
    if (c < '0' || c > '9') { ff_ungetc(f, c); return false; }
    u16 v = 0;
    while (c >= '0' && c <= '9') { v = (u16)(v * 10u + (u16)(c - '0')); c = ff_getc(f); }
    ff_ungetc(f, c);
    *out = (s16)(neg ? (u16)(0u - v) : v);            /* MSC int is 16 bits */
    return true;
}

/* ================================================================================================
 * Segment 0000
 * ============================================================================================== */

/* PORT: ticks per dissolve step. The original runs the steps at CPU speed; the graphics layer does not
 * pace them (platform.md §7: "The dissolves are paced by their callers"). */
#define REVEAL_STEP_TICKS 4

/* 0000:0000 screen_reveal — game_flow.md §4.2 (verified) */
s16 screen_reveal(s16 mode)
{
    if (mode != 0) select_screen();
    if (mode == 3) {
        for (u8 i = 0; i < 4; i++) {
            gfx_dissolve4(flow_page_sprite(), i);
            flow_pace(REVEAL_STEP_TICKS);            /* PORT */
        }
        return 0;
    }
    for (u8 i = 0; i < 8; i++) {
        gfx_dissolve8(flow_page_sprite(), i);
        flow_pace(REVEAL_STEP_TICKS);                /* PORT */
        if (mode == 1 || mode == 0) {
            s16 k = (s16)getkey();
            if (k != 0) return k;
        }
    }
    return 0;
}

/* 0000:0090 message_box — game_flow.md §4.3 (verified) */
s16 message_box_0000_0090(u16 msg_ds, s16 timed)
{
    u16 st = ds_stack_alloc(2 * GFX_SAVE_WORDS);      /* [bp-0x36] */
    gfx_targets_save(st);
    select_screen();
    FarPtr save = gfx_create_buffer(0x140, 0x28, 0x0F);
    grab_into_sprite_raw(gfx_desc_sprite(save), 0, 0x50);
    gfx_fill_rect(0, 0x50, 0x140, 0x28, 0);
    draw_rect_outline(0, 0x50, 0x13F, 0x77, 0xFFFF);
    gfx_set_text_colours(0x0F, 0);
    draw_text_centered(msg_ds, 0x5B);
    draw_text_centered(0x70A6, 0x65);                                /* "Press any key to continue." */
    s16 k = (s16)(timed ? getkey_timeout(3000) : getkey_wait());
    blit_copy_raw(gfx_desc_sprite(save), 0, 0x50);
    gfx_free_buffer(save);
    gfx_targets_restore(st);
    ds_stack_release(st);
    return (k == KEY_ESC || k == 0) ? k : 0;
}

/* 0000:01b0 question_box — game_flow.md §4.3 (verified; only the dropped install code calls it) */
s16 question_box(s16 unused, u16 msg_ds)
{
    (void)unused;
    u16 st = ds_stack_alloc(2 * GFX_SAVE_WORDS);
    gfx_targets_save(st);
    select_screen();
    FarPtr save = gfx_create_buffer(0x140, 0x28, 0x0F);
    grab_into_sprite_raw(gfx_desc_sprite(save), 0, 0x50);
    gfx_fill_rect(0, 0x50, 0x140, 0x28, 0);
    draw_rect_outline(0, 0x50, 0x13F, 0x77, 0xFFFF);
    gfx_set_text_colours(0x0F, 0);
    draw_text_centered(msg_ds, 0x60);
    s16 k = (s16)getkey_wait();
    blit_copy_raw(gfx_desc_sprite(save), 0, 0x50);
    gfx_free_buffer(save);
    gfx_targets_restore(st);
    ds_stack_release(st);
    return k;
}

/* 0000:0297 ensure_disk — game_flow.md §4.16, §9.1.
 * PORT: no disk prompts, no DISKID.DAT checks; always succeeds. DS:8644 receives the plain name
 * (no "X:" drive prefix); the platform file layer resolves it case-insensitively in the game directory. */
s16 ensure_disk_c(const char *name, s16 type, s16 mode)
{
    (void)type; (void)mode;
    char *dst = DSTR(DS_disk_path);
    size_t i = 0;
    for (; name[i] && i < 19; i++) dst[i] = name[i];  /* DS:8644..8657 */
    dst[i] = 0;
    return 0;
}

s16 ensure_disk(u16 name_ds, s16 type, s16 mode)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%s", DSTR(name_ds));
    return ensure_disk_c(tmp, type, mode);
}

/* 0000:0651 cars_select_by_name — game_flow.md §4.4 (verified) */
void cars_select_by_name(void)
{
    DSS(DS_car_idx) = -1;
    DSS(DS_opp_idx) = -1;
    for (s16 i = 0; i < DSS(DS_ncars); i++) {         /* last match wins */
        if (flow_stricmp(car_rec(i), DS_player_car_code) == 0) DSS(DS_car_idx) = i;
        if (flow_stricmp(car_rec(i), DS_opp_car_code) == 0) DSS(DS_opp_idx) = i;
    }
    if (DSS(DS_car_idx) < 0) DSS(DS_car_idx) = 0;
    strcpy(DSTR(DS_player_car_code), DSTR(car_rec(DSS(DS_car_idx))));
    DSS(DS_car_disk) = car_disk(DSS(DS_car_idx));
    if (DSS(DS_opp_idx) < 0) DSS(DS_opp_idx) = (DSS(DS_ncars) > 1) ? 1 : 0;
    strcpy(DSTR(DS_opp_car_code), DSTR(car_rec(DSS(DS_opp_idx))));
    DSS(DS_opp_disk) = car_disk(DSS(DS_opp_idx));
}

/* 0000:0730 scenery_select_by_name — game_flow.md §4.4 (verified) */
void scenery_select_by_name(void)
{
    DSS(DS_scn_idx) = -1;
    for (s16 i = 0; i < DSS(DS_nscenes); i++)
        if (flow_stricmp(scn_rec(i), DS_scn_code) == 0) DSS(DS_scn_idx) = i;
    if (DSS(DS_scn_idx) < 0) {
        strcpy(DSTR(DS_scn_code), DSTR(DS_scenery_table));
        DSS(DS_scn_idx) = 0;
    }
    strcpy(DSTR(DS_scn_code), DSTR(scn_rec(DSS(DS_scn_idx))));
    DSS(DS_scn_disk) = scn_disk(DSS(DS_scn_idx));
}

/* ================================================================================================
 * 0000:07b3 main — game_flow.md §4.1 (verified against the disassembly 0000:07b3-0c00)
 * ============================================================================================== */

static void flow_register_code(void)
{
    codeptr_register(FN_showroom_tick, showroom_tick);
}

int game_main(void)
{
    s16 prompted = 0;
    s16 r;

    flow_register_code();                            /* PORT: code pointers stored by this module */
    mem_init_default();                              /* 06c9:6f86 */
    /* PORT: bios_ticks() for the protection timing check dropped (§9.2).
     * PORT: argv[1] == "herc" is not supported: always the EGA branch. */
    gfx_init_ega();                                  /* 06c9:90e8 */
    kbd_set_getkey_fn(codeptr_far(FN_getkey_menu));  /* 06c9:6a17(06c9:65c5) */
    hotkeys_install();                               /* 16fc:0002 */
    kbd_install();                                   /* 06c9:6860 */
    timer_install_div((s16)PIT_DIV_GAME);            /* 06c9:6059(0x2E9C) */
    timer_add_routine(codeptr_far(FN_music_tick));   /* 06c9:614c(06c9:75ea) */
    gfx_video_hook();                                /* 06c9:5d00 */
    ds_far_wr(DS_voices, load_raw(0x718C));          /* "voices.bin" */
    ds_far_wr(DS_songs, load_raw(0x7197));           /* "songs.bin" */
    music_set_voices(ds_far(DS_voices));
    music_play(ds_far(DS_songs), 1);
    ds_far_wr(DS_page_buf_desc, gfx_create_buffer(0x140, 0xC8, 0x0F));

    /* PORT: disk identity, hard-disk PLAY scan, "Please insert MASTER or PLAY Disk." and the copy
     * protection are dropped (§9.1-9.3). The game runs from a MASTER disk in drive C:, protection passed. */
    {
        s16 drv = 3;
        DSS(DS_start_drive) = drv;
        DSS(DS_boot_disk_type) = 0;
        DSW(DS_drive_disk_type + 2 * drv) = 1;
        DSS(DS_drive_of_type) = drv;
        DSB(DS_protect_drive) = (u8)(drv - 1);
        DSW(DS_protect_failed) = 0;                  /* copyprot_check() == 0 */
        DSB(DS_protect_sabotage) = 0;
        DSS(DS_num_drives) = drv;
        DSS(DS_first_hd_drive) = drv;
    }
    for (;;) {                                       /* 0000:0950 */
        s16 t = 0;                                   /* MASTER */
        s16 drv = DSS(DS_start_drive);
        DSS(DS_data_disk) = t;
        DSW(DS_drive_disk_type + 2 * drv) = (u16)(t + 1);
        DSS(DS_drive_of_type + 2 * t) = drv;
        if (select_load(DSS(DS_data_disk)) != 0) goto shutdown;
        DSS(DS_ncars) = 0;
        DSS(DS_nscenes) = 0;
        cars_load(DSS(DS_data_disk), 1);
        if (DSS(DS_ncars) != 0) {
            scenes_load(DSS(DS_data_disk), 1);
            if (DSS(DS_nscenes) != 0) break;
        }
        if (DSS(DS_data_disk) == DSS(DS_boot_disk_type)) {
            /* PORT: the original exits silently (§9.1.2) */
            fatal("CARS.DAT and SCENES.DAT must list at least one car and one scenery.");
        }
        forget_play_disk();
    }
    DSS(DS_ncars_main) = DSS(DS_ncars);
    cars_select_by_name();
    scenery_select_by_name();
    if (hisc_load() != 0) goto shutdown;

    for (;;) {                                       /* 0000:0ab9 */
        r = intro_sequence();
        if (r == KEY_ESC) {
            gfx_fill_rect(0x28, 0x50, 0x118, 0x28, 0);
            draw_rect_outline(0x28, 0x50, 0x117, 0x77, 0xFFFF);
            gfx_set_text_colours(0x0F, 0);
            draw_text_centered(0x71D3, 0x60);        /* "Exit to DOS (Y/N)?" */
            do r = (s16)getkey_timeout(3000); while (r == KEY_ESC);
            if (r == 'y' || r == 'Y') {
                select_save(DSS(DS_data_disk));
                goto shutdown;
            }
            continue;
        }
        DSW(DS_demo_mode) = (r == 0) ? 1 : DSW(DS_protect_failed);
        if (!prompted) {
            if ((DSS(DS_drive_of_type + 2) != 0 || DSS(DS_drive_of_type + 4) != 0)
                && select_load(DSS(DS_data_disk)) != 0)
                goto shutdown;
            if (DSS(DS_drive_of_type + 6) != 0 && DSS(DS_boot_disk_type) != 3) {
                if (select_load(3) == 0) DSS(DS_data_disk) = 3;
                else forget_play_disk();
            }
            DSW(DS_catalog_dirty) = 1;
            if (catalog_reload() != 0) goto shutdown;
            prompted = 1;
        }
        /* PORT: "if (elapsed > 500) protect_failed = 1" dropped (§9.2). */
        if (main_menu() != 0) goto shutdown;
    }

shutdown:
    timer_restore();                                 /* 06c9:610a */
    kbd_restore();                                   /* 06c9:68c4 */
    gfx_shutdown();                                  /* 06c9:642e (no Hercules: 06c9:5fbc not used) */
    return 0;
}

/* ================================================================================================
 * Segment 0432: catalogue
 * ============================================================================================== */

/* 0432:0006 name_to_field — game_flow.md §2a (verified; install code only) */
void name_to_field(u16 dst_ds, u16 src_ds)
{
    strcpy(DSTR(dst_ds), DSTR(src_ds));
    for (s16 i = 0x11; i > 0 && DSB(dst_ds + i) == ' '; i--) DSB(dst_ds + i) = 0;
    for (u16 i = 0; DSB(dst_ds + i) != 0; i++)
        if (DSB(dst_ds + i) == ' ') DSB(dst_ds + i) = '_';
}

/* 0432:0061 field_to_name — game_flow.md §2a (verified) */
void field_to_name(u16 s_ds)
{
    u16 i = 0;
    for (; DSB(s_ds + i) != 0; i++)
        if (DSB(s_ds + i) == '_') DSB(s_ds + i) = ' ';
    for (; (s16)i < 0x12; i++) DSB(s_ds + i) = ' ';
    DSB(s_ds + i) = 0;
}

/* 0432:00a9 cars_load / 0432:01b2 scenes_load — game_flow.md §4.4 (verified) */
static s16 catalog_file_load(s16 disk, s16 flag, bool scenes)
{
    const u16 count_ds = scenes ? DS_nscenes : DS_ncars;
    const u16 name_ds  = scenes ? 0x7E1C : 0x7DEC;          /* "scenes.dat" / "cars.dat" */
    const u16 err_ds   = scenes ? 0x7E29 : 0x7DF7;          /* "Cannot open ... for read." */
    FlowFile f;

    if (ensure_disk(name_ds, disk, 0) != 0) return KEY_ESC;
    if (!flow_fopen_read(&f, DSTR(DS_disk_path))) {
        message_box_0000_0090(err_ds, 0);
        return KEY_ESC;
    }
    for (;;) {
        s16 n = DSS(count_ds);
        if (f.eof) break;                                    /* FILE._flag & _IOEOF */
        if (n >= FLOW_MAX_ENTRIES) break;                    /* PORT: 31 in the original */
        u16 rec = scenes ? scn_rec(n) : car_rec(n);
        /* fscanf(f, "%s %s") / fscanf(f, "%s %s %d") */
        if (flow_scan_str(&f, rec, 5 + 16) && flow_scan_str(&f, (u16)(rec + 5), 0x15) && scenes) {
            s16 v;
            if (flow_scan_int(&f, &v)) DSS(rec + 0x1E) = v;
        }
        if (DSB(rec) == 0) break;
        if (n != 0 && flag != 0) {
            /* stricmp(new code, table[i].code) for every earlier entry; the result is unused */
        }
        DSS(rec + 0x1A) = disk;
        DSS(rec + 0x1C) = 0;
        field_to_name((u16)(rec + 5));
        DSS(count_ds) = (s16)(n + 1);
    }
    flow_fclose(&f);
    return 0;
}

s16 cars_load(s16 disk, s16 flag)   { return catalog_file_load(disk, flag, false); }
s16 scenes_load(s16 disk, s16 flag) { return catalog_file_load(disk, flag, true); }

/* 0432:02bf select_load — game_flow.md §4.4, §5.3 (verified) */
s16 select_load(s16 disk)
{
    FlowFile f;
    if (ensure_disk(0x7E53, disk, 0) != 0) return KEY_ESC;           /* "select.dat" */
    if (flow_fopen_read(&f, DSTR(DS_disk_path))) {
        /* fscanf(f, "%d %d %d %s %s %s", &DS:8A1E, &DS:8A20, &DS:8A22, car, opp, scn) */
        s16 drv[3];
        u16 codes[3] = { DS_player_car_code, DS_opp_car_code, DS_scn_code };
        int i = 0;
        for (; i < 3; i++) {
            if (!flow_scan_int(&f, &drv[i])) break;
            DSS(DS_drive_of_type + 2 + 2 * i) = 0;              /* PORT: extra disk drives ignored (§9.1.3) */
        }
        if (i == 3)
            for (int j = 0; j < 3; j++)
                if (!flow_scan_str(&f, codes[j], 6)) break;     /* 6-byte fields (DS:8428, 90B0, 9212) */
        flow_fclose(&f);
        return 0;
    }
    DSS(DS_drive_of_type + 2) = 0;
    DSS(DS_drive_of_type + 4) = 0;
    DSS(DS_drive_of_type + 6) = 0;
    strcpy(DSTR(DS_player_car_code), DSTR(0x7E72));                /* "F40" */
    strcpy(DSTR(DS_opp_car_code), DSTR(0x7E76));                   /* "P959" */
    strcpy(DSTR(DS_scn_code), DSTR(0x7E7B));                       /* "TDS2" */
    return 0;
}

/* 0432:0376 select_save — game_flow.md §4.4 (verified) */
void select_save(s16 disk)
{
    s16 r = (disk == 3) ? ensure_disk(0x7E80, 3, 1) : ensure_disk(0x7E8B, disk, 0);   /* "select.dat" */
    if (r != 0) return;
    char *path = host_game_path(DSTR(DS_disk_path), true);
    FILE *fp = path ? fopen(path, "wb") : NULL;
    host_free(path);
    if (!fp) {
        message_box_0000_0090(0x7E98, 0);                           /* "Cannot open select.dat for write" */
        return;
    }
    /* fprintf(f, "%d %d %d %s %s %s", ...) — PORT: the drive fields are written as 0 0 0 (§9.1.3) */
    fprintf(fp, "%d %d %d %s %s %s", 0, 0, 0,
            DSTR(DS_player_car_code), DSTR(DS_opp_car_code), DSTR(DS_scn_code));
    fclose(fp);
}

/* 0432:1d25 forget_play_disk — game_flow.md §2a (verified) */
void forget_play_disk(void)
{
    DSW(DS_drive_disk_type + 2 * DSW(DS_drive_of_type + 6)) = 0;
    DSS(DS_drive_of_type + 6) = 0;
    DSS(DS_data_disk) = 0;
    DSS(DS_drive_of_type) = DSS(DS_start_drive);
}

/* 0432:1d44 catalog_reload — game_flow.md §4.4 (verified). The extra CAR / SCENERY disk branches are
 * kept; with the port's select.dat handling their drive fields are always 0. */
s16 catalog_reload(void)
{
    for (;;) {
        if (DSW(DS_catalog_dirty) == 0) return 0;
        DSS(DS_ncars) = 0;
        DSS(DS_nscenes) = 0;
        if (cars_load(DSS(DS_data_disk), 1) != 0) {
            if (DSS(DS_data_disk) == DSS(DS_boot_disk_type)) return 1;
            forget_play_disk();
            if (cars_load(DSS(DS_data_disk), 1) != 0) return 1;
        }
        DSS(DS_ncars_main) = DSS(DS_ncars);
        if (scenes_load(DSS(DS_data_disk), 1) != 0) {
            if (DSS(DS_data_disk) == DSS(DS_boot_disk_type)) return 1;
            forget_play_disk();
            if (scenes_load(DSS(DS_data_disk), 1) != 0) return 1;
        }
        if (DSS(DS_ncars) != 0 && DSS(DS_nscenes) != 0) break;
        if (DSS(DS_data_disk) == DSS(DS_boot_disk_type)) return 1;
        message_box_0000_0090(0x8175, 0);            /* "Play Disk needs both cars and scenery!" */
        forget_play_disk();
    }
    if (DSS(DS_drive_of_type + 2) != 0) {            /* extra CAR disk */
        if (cars_load(1, 1) == 0) {
            DSB(DS_protect_drive) = (u8)(DSS(DS_drive_of_type + 2) - 1);
            goto cars_ok;                            /* PORT: copyprot_check() == 0 */
        }
        DSW(DS_drive_disk_type + 2 * DSW(DS_drive_of_type + 2)) = 0;
        DSS(DS_drive_of_type + 2) = 0;
    }
cars_ok:
    if (DSS(DS_drive_of_type + 4) != 0) {            /* extra SCENERY disk */
        if (scenes_load(2, 1) == 0) {
            DSB(DS_protect_drive) = (u8)(DSS(DS_drive_of_type + 4) - 1);
            goto scn_ok;                             /* PORT: copyprot_check() == 0 */
        }
        DSW(DS_drive_disk_type + 2 * DSW(DS_drive_of_type + 4)) = 0;
        DSS(DS_drive_of_type + 4) = 0;
    }
scn_ok:
    cars_select_by_name();
    scenery_select_by_name();
    hisc_load();
    DSW(DS_catalog_dirty) = 0;
    return 0;
}

/* 0432:1ea4 install_menu — game_flow.md §9.1.5.
 * PORT: the install menu (Car / Scenery / Play disk handling, Make Play Disk, Copy Cars / Scenery) is
 * dropped. Selecting the icon returns to the main menu with the catalogue unchanged. */
s16 install_menu(void)
{
    return 0;
}
