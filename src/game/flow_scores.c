/* game_flow segment 0645: <scn>hisc.dat, name entry, top-score table — port/spec/game_flow.md §4.15, §5.4. */
#include <stdio.h>
#include <string.h>

#include "flow.h"
#include "../host.h"
#include "../platform/gfx.h"
#include "../platform/input.h"
#include "../platform/res.h"

/* Checksum of the record tables (inline in 0645:0000 and 0645:0186; verified): 32-bit sum of the six
 * scores, then per stage the time (sign-extended), average, score, cumulative time and score. */
u32 hisc_checksum(void)
{
    u32 sum = 0;
    for (s16 i = 0; i < 6; i++) sum += DSL(top_rec(i));
    for (s16 i = 0; i < 10; i++) {
        sum += (u32)(s32)DSS(DS_best_time + 2 * i);
        sum += DSL(DS_best_avg + 4 * i);
        sum += DSL(DS_best_score + 4 * i);
        sum += DSL(DS_best_cum_time + 4 * i);
        sum += DSL(DS_best_cum_score + 4 * i);
    }
    return sum;
}

/* 0645:0000 hisc_load — game_flow.md §4.15 (verified) */
s16 hisc_load(void)
{
    char name[26];
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_scn_code), DSTR(0x8200)), DSS(DS_scn_disk), 0) != 0)
        return KEY_ESC;                                                     /* "hisc.dat" */
    char *path = host_game_path(DSTR(DS_disk_path), false);
    FILE *fp = path ? fopen(path, "rb") : NULL;
    host_free(path);
    if (fp) {
        /* fclose; 06c9:6aa2(disk_path, DS:90B8) — PORT: reads at most the 0x154 bytes of the table
         * (the original loads the whole file without a size check) */
        size_t got = fread(mp(DGROUP, DS_hisc_block), 1, HISC_SIZE, fp);
        (void)got;
        fclose(fp);
        if (hisc_checksum() == DSL(DS_hisc_checksum)) return 0;
    }
    for (s16 i = 0; i < 6; i++) {
        DSL(top_rec(i)) = 0;
        DSB(top_car(i)) = 0;
        DSB(top_name(i)) = 0;
    }
    for (s16 i = 0; i < 10; i++) {
        DSW(DS_best_time + 2 * i) = 0;
        DSL(DS_best_avg + 4 * i) = 0;
        DSL(DS_best_score + 4 * i) = 0;
        DSL(DS_best_cum_time + 4 * i) = 0;
        DSL(DS_best_cum_score + 4 * i) = 0;
    }
    return 0;
}

/* 0645:0186 hisc_save — game_flow.md §4.15 (verified) */
void hisc_save(void)
{
    char name[26];
    DSL(DS_hisc_checksum) = hisc_checksum();
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_scn_code), DSTR(0x820B)), DSS(DS_scn_disk), 0) == 0)
        write_file_or_die(DS_disk_path, ds_ptr(DS_hisc_block), HISC_SIZE);  /* "hisc.dat", 06c9:7866 */
}

/* 0645:0271 name_entry_screen — game_flow.md §4.15 (verified) */
s16 name_entry_screen(void)
{
    char name[26];
    select_screen();
    gfx_clear_clip(0);
    if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(DS_player_car_code), DSTR(0x8214)),
                      DSS(DS_car_disk), 0) != 0)
        return KEY_ESC;                                                     /* "rear" */
    FarPtr a = load_shapes(DS_disk_path);
    blit_copy_own(res_find(a, 0x8219));                                     /* "logL" */
    mem_release_cache(a);
    gfx_set_text_colours(0x0F, 0);
    draw_text_centered(0x821E, 0x96);              /* "You have qualified as one of" */
    draw_text_centered(0x823B, 0xA0);              /* "THE DUEL: Test Drive II's best drivers." */
    gfx_draw_text(0x8263, 0x14, 0xB4);             /* "Enter your name:" */
    draw_rect_outline(0xB2, 0xAF, 0x13C, 0xBE, 0xFFFF);
    input_text_line(DS_player_name, 15, 0xB8, 0xB4, 12000);   /* result ignored */
    return 0;
}

/* 0645:0389 hisc_insert — game_flow.md §4.15 (verified) */
s16 hisc_insert(void)
{
    s32 total = DSSL(DS_total_score);
    s16 i;
    for (i = 0; i < 6; i++)
        if ((s32)DSL(top_rec(i)) < total) break;
    if (i == 6) return (s16)(u16)total;            /* undefined in the original (AX = low word of the score) */
    if (name_entry_screen() != 0) return KEY_ESC;
    for (s16 j = 5; j > i; j--) {
        DSL(top_rec(j)) = DSL(top_rec(j - 1));
        strcpy(DSTR(top_car(j)), DSTR(top_car(j - 1)));
        strcpy(DSTR(top_name(j)), DSTR(top_name(j - 1)));
    }
    DSL(top_rec(i)) = DSL(DS_total_score);
    strcpy(DSTR(top_car(i)), DSTR(DS_player_car_code));
    strcpy(DSTR(top_name(i)), DSTR(DS_player_name));
    hisc_save();
    return 0;
}

/* 0645:0470 hisc_show — game_flow.md §4.15 (verified against the disassembly) */
s16 hisc_show(s32 timeout)
{
    char t[72], name[26];
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    gfx_set_text_colours(0x0F, 0);
    draw_text_centered(0x8274, 0);                 /* "THE DUEL: TEST DRIVE II TOP SCORES" */
    snprintf(t, sizeof t, "%s", DSTR(scn_name(DSS(DS_scn_idx))));
    for (s16 k = 0x11; ; ) {                       /* trim trailing spaces from index 17 */
        if (t[k] != ' ') break;
        t[k] = 0;
        k--;
        if (k <= 0) break;
    }
    draw_text_centered_str(t, 10);
    for (s16 i = 0; i < 6; i++) {
        s16 c = -1;
        for (s16 j = 0; j < DSS(DS_ncars); j++) {
            if (flow_stricmp(car_rec(j), top_car(i)) == 0) { c = j; break; }
        }
        if (c != -1 && i < 4) {
            if (ensure_disk_c(flow_concat(name, sizeof name, DSTR(car_rec(c)), DSTR(0x8297)), car_disk(c), 0) != 0)
                return KEY_ESC;                                             /* "rear" */
            FarPtr a = load_shapes(DS_disk_path);
            blit_copy_raw(res_find(a, 0x829C), 0, (s16)(i * 0x26 + 0x17));  /* "logS" */
            mem_release_cache(a);
        }
        s16 y = (i < 4) ? (s16)(i * 0x26 + 0x21) : (s16)(i * 10 + 0x7D);
        snprintf(t, sizeof t, "%ld  ", (long)(s32)DSL(top_rec(i)));         /* DS:82A1 */
        gfx_draw_text_str(t, 0x50, (u16)y);
        gfx_draw_text(top_name(i), 0x90, (u16)y);
    }
    if (timeout == 1500) {
        snprintf(t, sizeof t, "Your Score:  %ld", (long)DSSL(DS_total_score));  /* DS:82A7 */
        draw_text_centered_str(t, 0xC0);
    }
    s16 r = screen_reveal(1);
    if (r != 0) return r;
    return (s16)getkey_timeout((u32)timeout);
}

/* 0645:06a8 hisc_check — game_flow.md §4.15 (verified) */
s16 hisc_check(void)
{
    if ((s32)DSL(top_rec(5)) < DSSL(DS_total_score) && DSW(DS_demo_mode) == 0 && hisc_insert() != 0)
        return KEY_ESC;
    return hisc_show(1500);
}
