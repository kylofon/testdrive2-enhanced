#pragma once
/* game_flow (segments 0000-0645 of TD2EGA.EXE): main, intro screens, menus, showroom, catalogue files,
 * run_game, results, record book, high scores, police ending — port/spec/game_flow.md.
 * Cross-module prototypes are in game.h; this header declares the module's own functions (named as in
 * symbols.h) and a few port helpers (flow_*). All game state is in mem[] (symbols.h offsets). */
#include "game.h"

/* ---- common constants (game_flow.md §4) */
#define KEY_ESC   0x1B
#define KEY_ENTER 0x0D
#define KEY_SPACE 0x20
#define KEY_UP    0x4800
#define KEY_DOWN  0x5000
#define KEY_LEFT  0x4B00
#define KEY_RIGHT 0x4D00
#define KEY_F10   0x4400

#define CAR_REC_SIZE   30           /* DS:8D24 records (§5.1) */
#define SCN_REC_SIZE   32           /* DS:865C records (§5.2) */
#define HISC_SIZE      0x154        /* DS:90B8 file image (§5.4) */
#define TOP_REC_SIZE   26           /* top-6 record: s32 score, char car[5], char name[17] */
#define FLOW_MAX_ENTRIES 30         /* PORT: the loaders accept 31 entries; the 31st overlaps other globals */

/* DGROUP string / buffer at DS:o (the original passes these near pointers). */
#define DSTR(o) ((char *)mp(DGROUP, (u16)(o)))

static inline u16 car_rec(s16 i)  { return (u16)(DS_car_table + CAR_REC_SIZE * i); }       /* code at +0 */
static inline u16 car_name(s16 i) { return (u16)(car_rec(i) + 5); }
static inline s16 car_disk(s16 i) { return DSS(car_rec(i) + 0x1A); }
static inline u16 scn_rec(s16 i)  { return (u16)(DS_scenery_table + SCN_REC_SIZE * i); }   /* code at +0 */
static inline u16 scn_name(s16 i) { return (u16)(scn_rec(i) + 5); }
static inline s16 scn_disk(s16 i) { return DSS(scn_rec(i) + 0x1A); }
static inline s16 scn_stages(s16 i) { return DSS(scn_rec(i) + 0x1E); }
static inline u16 top_rec(s16 i)  { return (u16)(DS_hisc_block + TOP_REC_SIZE * i); }      /* score s32 at +0 */
static inline u16 top_car(s16 i)  { return (u16)(top_rec(i) + 4); }
static inline u16 top_name(s16 i) { return (u16)(top_rec(i) + 9); }

/* Off-screen page DS:8CA2 (a target descriptor) and the sprite header it describes. */
static inline FarPtr flow_page_desc(void) { return ds_far(DS_page_buf_desc); }
FarPtr flow_page_sprite(void);
/* Sprite header fields (+0 width in bytes, +2 height, +8 x, +0x0A y). */
static inline s16 spr_w(FarPtr s) { return (s16)rd16(s.seg, s.off); }
static inline s16 spr_h(FarPtr s) { return (s16)rd16(s.seg, (u16)(s.off + 2)); }
static inline s16 spr_x(FarPtr s) { return (s16)rd16(s.seg, (u16)(s.off + 8)); }
static inline s16 spr_y(FarPtr s) { return (s16)rd16(s.seg, (u16)(s.off + 0x0A)); }

/* ---- port helpers (flow.c) */
/* MSC long arithmetic (_aldiv / _alrem): a zero divisor ends the program with R6003. */
s32  flow_aldiv(s32 a, s32 b);
s32  flow_alrem(s32 a, s32 b);
/* 32/16 IDIV after CWD of a 16-bit value (quotient / remainder as the original uses them). */
s16  flow_idiv(s16 a, s16 b);
s16  flow_imod(s16 a, s16 b);
/* 13a8:2b40 stricmp on two DGROUP strings. */
int  flow_stricmp(u16 a_ds, u16 b_ds);
/* strcpy(buf, a); strcat(buf, b) — the original's name building into a stack local. */
char *flow_concat(char *buf, size_t n, const char *a, const char *b);
/* PORT: pacing for effects the original ran at CPU speed: present, then wait n timer ticks. */
void flow_pace(u32 ticks);
/* 16bb/0x94C7-style lookup of concatenated 4-character names into a C array (at most max entries). */
int  flow_find_list(FarPtr arc, const char *names, FarPtr *out, int max);

/* MSC text-mode stdio emulation over a game file (CR LF -> LF, ^Z = end of file). */
typedef struct { unsigned char *data; size_t len, pos; bool eof; } FlowFile;
bool flow_fopen_read(FlowFile *f, const char *name);
void flow_fclose(FlowFile *f);
/* fscanf conversions: %s into DGROUP (unchanged when no character was read) and %d (16-bit int). */
bool flow_scan_str(FlowFile *f, u16 dst_ds, u16 cap);
bool flow_scan_int(FlowFile *f, s16 *out);

/* ---- segment 0000 (flow.c) */
s16  screen_reveal(s16 mode);                                   /* 0000:0000 */
s16  message_box_0000_0090(u16 msg_ds, s16 timed);              /* 0000:0090 */
s16  question_box(s16 unused, u16 msg_ds);                      /* 0000:01b0 (install code only) */
s16  ensure_disk(u16 name_ds, s16 type, s16 mode);              /* 0000:0297 PORT: no disks */
s16  ensure_disk_c(const char *name, s16 type, s16 mode);       /* PORT: same for a C-string name */
void cars_select_by_name(void);                                 /* 0000:0651 */
void scenery_select_by_name(void);                              /* 0000:0730 */
/* int game_main(void);                                            0000:07b3 main (game.h) */

/* ---- segment 0432 (flow.c): catalogue */
void name_to_field(u16 dst_ds, u16 src_ds);                     /* 0432:0006 */
void field_to_name(u16 s_ds);                                   /* 0432:0061 */
s16  cars_load(s16 disk, s16 flag);                             /* 0432:00a9 */
s16  scenes_load(s16 disk, s16 flag);                           /* 0432:01b2 */
s16  select_load(s16 disk);                                     /* 0432:02bf */
void select_save(s16 disk);                                     /* 0432:0376 */
void forget_play_disk(void);                                    /* 0432:1d25 */
s16  catalog_reload(void);                                      /* 0432:1d44 */
s16  install_menu(void);                                        /* 0432:1ea4 PORT: does nothing */

/* ---- segments 00c0 / 010c / 0143 / 019e (flow_screens.c) */
s16  title_screen(void);                                        /* 00c0:0004 */
s16  accolade_screen(void);                                     /* 00c0:027a */
s16  intro_sequence(void);                                      /* 00c0:039c */
s16  dsi_logo_screen(void);                                     /* 010c:000e */
s16  credits(void);                                             /* 010c:0285 */
s16  showroom_intro(s16 unused);                                /* 0143:000a */
void showroom_tick(void);                                       /* 0143:019e timer routine */
s16  showroom_drive(const char *path, s16 x, s16 y, s16 anim, s16 reveal);   /* 0143:01e1 */
u16  menu_random_key(void);                                     /* 019e:0004 */
FarPtr car_slide(s16 car, s16 from, s16 to, FarPtr old);        /* 019e:009b */
s16  car_select(s16 car);                                       /* 019e:027d */
void scenery_select(void);                                      /* 019e:0527 */
s16  main_menu(void);                                           /* 019e:06c5 */

/* ---- segment 0267 (flow_stage.c) */
u16  plural_s(s16 n);                                           /* 0267:0004 -> DS string */
void results_line(const char *s, s16 x);                        /* 0267:0017 */
s16  stage_results(s16 kind);                                   /* 0267:0039 */
void results_overall(void);                                     /* 0267:118b */
void results_section_page(void);                                /* 0267:1224 */
s16  difficulty_screen(void);                                   /* 0267:139b */
s16  run_game(s16 mode);                                        /* 0267:15e4 */

/* ---- segment 0645 (flow_scores.c) */
u32  hisc_checksum(void);                                       /* inline in 0645:0000 / 0645:0186 */
s16  hisc_load(void);                                           /* 0645:0000 */
void hisc_save(void);                                           /* 0645:0186 */
s16  name_entry_screen(void);                                   /* 0645:0271 */
s16  hisc_insert(void);                                         /* 0645:0389 */
s16  hisc_show(s32 timeout);                                    /* 0645:0470 */
s16  hisc_check(void);                                          /* 0645:06a8 */
