#pragma once
/* PC-speaker sound — port of TD2EGA 06c9:6269 (effect stream player sfx_tick) and 06c9:758a..79c1
 * (SONGS.BIN music player music_tick and the sound API) (port/spec/platform.md §2.3, §3.3, §4.15,
 * §4.16, §5.5-5.7). State lives in DGROUP (DS:5EEE..5F2E, DS:65D2..66E4). The speaker (PIT channel 2
 * and the two low bits of port 61h) is modelled here and driven through host_speaker().
 *
 * sfx_tick and music_tick are timer routines: store them with timer_add_routine(codeptr_far(FN_x));
 * timer_init() registers them with codeptr. */
#include "../mem.h"

void sfx_tick(void);                        /* 06c9:6269 */
void music_tick(void);                      /* 06c9:75ea */

void music_play(FarPtr songs, u16 n);       /* 06c9:758a start song n if DS:5EFB == 3 (songs offset must be 0) */
void music_set_voices(FarPtr voices);       /* 06c9:75c8 DS:65F7 = seg, DS:65F9 = off */
void music_set_6625(u16 w6627, u16 w6625);  /* 06c9:75d9 (no callers) */

void sound_off(void);                       /* 06c9:7934 */
void sound_on(void);                        /* 06c9:7940 */
void sfx_set_loop(FarPtr s);                /* 06c9:7946 */
void sfx_clear_loop(void);                  /* 06c9:7971 */
void sfx_play(FarPtr s);                    /* 06c9:7977 */
s16  sfx_is_playing(void);                  /* 06c9:798f */
void sfx_stop(void);                        /* 06c9:7995 */
void sfx_play_if_enabled(FarPtr s);         /* 06c9:799c */
void music_on(void);                        /* 06c9:79bb */
void music_off(void);                       /* 06c9:79c1 */

/* ---- speaker hardware model (port helpers for the timer code) */
void spk_port61_and(u8 mask);               /* in al,61h; and al,mask; out 61h,al */
void spk_port61_or(u8 bits);                /* in al,61h; or al,bits; out 61h,al */
void spk_set_divisor(u16 div);              /* out 42h, lo; out 42h, hi */
