#pragma once
/* Timer ISR, routine list and tick helpers — port of TD2EGA 06c9:5d83..5d8d, 06c9:601c..6230,
 * 06c9:79c8..7a55, 06c9:c63e..c6c5 (port/spec/platform.md §2.3, §3.3, §4.13, §4.14, §4.27, §7).
 *
 * The host calls the ISR body at 99.9985 Hz from host_pump() (timer_init installs it). As in the original
 * it only does something while "INT 8" points at 06c9:61bf: the vector is kept in the real-mode IVT
 * inside mem[] (0000:0020), written by timer_install_* and timer_restore. Routine-list entries are far
 * code pointers in DS:5F12 (DS_timer_routines), resolved with codeptr_lookup(); every routine stored
 * there must be registered with codeptr_register (music_tick / sfx_tick are registered by timer_init).
 *
 * All wait helpers call host_pump() in their loops. */
#include "../mem.h"

void timer_init(void);                      /* PORT: host tick handler + codeptr registration (sound routines) */

void timer_install_drive(void);             /* 06c9:601c */
void timer_install_menu(void);              /* 06c9:603c (no callers) */
void timer_install_div(s16 div);            /* 06c9:6059 called with 0x2E9C */
void timer_install_div_countdown(s16 div);  /* 06c9:607a (no callers) */
void timer_install_common(u16 dx_div);      /* 06c9:6099 (tail of the four above) */
void timer_restore(void);                   /* 06c9:610a */
void timer_add_routine(FarPtr fn);          /* 06c9:614c fatal when all 5 slots are used */
void timer_remove_routine(FarPtr fn);       /* 06c9:6180 */
void timer_isr(void);                       /* 06c9:61bf body (EOI / chaining dropped) */
void timer_bios_chain(void);                /* 06c9:6230 (the BIOS call itself is dropped) */

u16  bios_ticks(void);                      /* 06c9:5d83 low word of 0040:006C (BIOS clock model) */
u16  bios_ticks_since(u16 t0);              /* 06c9:5d8d */

u32  ticks_get(void);                       /* 06c9:79c8 DS:5E8C (stops while paused) */
u32  ticks_since(u32 t);                    /* 06c9:79d2 */
u32  ticks_lap(void);                       /* 06c9:79ea */
void ticks_reset(void);                     /* 06c9:7a07 */
void deadline_set(u32 n);                   /* 06c9:7a10 DS:6818 = now + n */
void deadline_wait(void);                   /* 06c9:7a27 */
s16  deadline_passed(void);                 /* 06c9:7a3b */
void delay_ticks(u32 n);                    /* 06c9:7a55 */

u32  slow_ticks_get(void);                  /* 06c9:c692 DS:5E90 (20 Hz, runs while paused) */
void slow_deadline_set(u32 n);              /* 06c9:c69c DS:68D6 = slow + n */
void slow_deadline_wait(void);              /* 06c9:c6b3 */
void delay_slow(u32 n);                     /* 06c9:c6c5 */
/* getkey_until_slow_deadline (06c9:c63e) and wait_key_slow (06c9:c65c) are in input.h. */
