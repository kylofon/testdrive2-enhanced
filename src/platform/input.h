#pragma once
/* Keyboard, hotkeys, getkey family, joystick, driving input and the text-line editor — port of TD2EGA
 * 06c9:5d1e, 06c9:645a..6a6a, 06c9:c63e/c65c, 16aa:0002, 16bb (port/spec/platform.md §2.2, §3.2,
 * §4.6-4.10, §4.25, §5.10).
 *
 * kbd_install() puts kbd_int9_isr on host_set_scan_handler (the INT 9 hook); the key-down table, the
 * translation tables and the one-key buffer are the game's own (DS:62B6..64DB). INT 16h is emulated by
 * kbd_int16_isr / kbd_peek / kbd_read on top of that buffer. The getkey function pointer DS:64DC and the
 * hotkey table DS:6048 hold code addresses as in the original (far / near-in-06c9); input_init()
 * registers every function that can be stored there. The joystick is the host gamepad.
 *
 * All waiting loops call host_pump(). */
#include "../mem.h"

void input_init(void);                      /* PORT: codeptr registration (getkey_raw, getkey_menu, hotkeys) */

/* ---- INT 9 / INT 16h layer */
void kbd_install(void);                     /* 06c9:6860 */
void kbd_restore(void);                     /* 06c9:68c4 */
void kbd_int9_isr(u8 xt_code);              /* 06c9:6907 (host scan handler: port 60h value) */
u16  kbd_int16_isr(u16 ax, bool *zf);       /* 06c9:69b7 AH=0 read+clear, AH=1 peek (ZF), AH=2 shift, else 0 */
bool kbd_peek(u16 *key);                    /* PORT helper: int 16h AH=01h, true if a key (NZ) */
u16  kbd_read(void);                        /* PORT helper: int 16h AH=00h (does not block) */
u8   key_is_down(u16 scan);                 /* 06c9:69f2 (no callers) */

/* ---- hotkeys */
u16  kbd_dispatch(u16 key);                 /* 06c9:6528 hotkey handler -> 0, else key */
void hotkey_set(u16 key, u16 off, u16 seg_unused);   /* 06c9:6585 handler = near offset in 06c9 */
void hotkey_joystick_on(void);              /* 06c9:645a Ctrl-J */
void hotkey_keyboard_on(void);              /* 06c9:646f Ctrl-K */
void hotkey_sound_toggle(void);             /* 06c9:6488 Ctrl-S */
void hotkey_music_toggle(void);             /* 06c9:64c7 Ctrl-Q */
void hotkey_pause(void);                    /* 06c9:6508 Ctrl-P */
void hotkey_exit(void);                     /* 06c9:6518 Ctrl-X */
void wait_key_paused(void);                 /* 06c9:5d1e (no direct callers) */

/* ---- getkey family. Keys: ASCII in AL with AH = 0, or extended scan << 8 (platform.md §5.10). */
u16    getkey(void);                        /* 06c9:69fe far call through DS:64DC */
u16    getkey_raw(void);                    /* 06c9:6a03 */
u16    getkey_menu(void);                   /* 06c9:65c5 keys with hotkeys, joystick edges */
u16    getkey_drive(void);                  /* 06c9:6601 */
void   kbd_set_getkey_fn(FarPtr fn);        /* 06c9:6a17 e.g. codeptr_far(FN_getkey_menu) */
FarPtr kbd_get_getkey_fn(void);             /* 06c9:6a28 */
u16    getkey_wait(void);                   /* 06c9:6a30 */
u16    kbd_flush(void);                     /* 06c9:6a3b returns 0 */
u16    getkey_until_deadline(void);         /* 06c9:6a4a until ticks >= DS:6818 (deadline_set) */
u16    getkey_timeout(u32 n);               /* 06c9:6a6a */
u16    getkey_until_slow_deadline(void);    /* 06c9:c63e until slow >= DS:68D6 (slow_deadline_set) */
u16    wait_key_slow(u32 n);                /* 06c9:c65c (no callers) */

/* ---- driving input and joystick */
u16  input_drive_bits(void);                /* 06c9:6620 held keys -> 1 up 2 down 4 right 8 left 10 A 20 B, else joy */
u16  joy_read(void);                        /* 06c9:6686 */
void joy_calib_reset(void);                 /* 06c9:680a */
u16  joy_dir_index(u16 bits);               /* 06c9:6828 DS:629E[bits & 15] */
s16  joy_analog_x(void);                    /* 06c9:6839 (no callers) */
s16  joy_analog_y(void);                    /* 06c9:684c (no callers) */

/* ---- C helpers */
s16  toupper_c(s16 c);                      /* 16aa:0002 low byte, a-z -> A-Z, sign-extended */

/* ---- 16bb: one-line text editor (high-score name). buf is a DGROUP buffer of maxlen + 1 bytes.
 * Returns the exit key (0x0D, 0x1B, 0x09, 0x4800, 0x5000) or 0 on timeout (timeout 0 = none). */
s16  edit_text_line(u16 buf_ds, s16 maxlen, s16 x, s16 y, u32 timeout);    /* 16bb:000a */
s16  input_text_line(u16 buf_ds, s16 maxlen, s16 x, s16 y, u32 timeout);   /* 16bb:02a8 */
void edit_cursor_xor(void);                 /* 16bb:02e6 */
