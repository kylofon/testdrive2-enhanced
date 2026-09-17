/* Keyboard, hotkeys, getkey family, joystick, driving input and the text-line editor — port of TD2EGA
 * 06c9:5d1e, 06c9:645a..6a6a, 06c9:c63e/c65c, 16aa:0002 and 16bb (port/spec/platform.md §4.6-4.10,
 * §4.25, §5.10; checked against the disassembly).
 *
 * The real-mode vectors 9 and 16h are kept in mem[] (0000:0024, 0000:0058). The host scan handler runs
 * the game's INT 9 body only while vector 9 points at 06c9:6907, and INT 16h calls go to the game's
 * handler only while vector 16h points at 06c9:69b7; otherwise the host's BIOS keyboard buffer answers,
 * as the BIOS would. */
#include "input.h"

#include "../codeptr.h"
#include "../host.h"
#include "../symbols.h"
#include "gfx.h"
#include "prompts.h"
#include "timer.h"

#define INT9_OFF   0x6907
#define INT16_OFF  0x69B7
#define IVT9       0x0024
#define IVT16      0x0058
#define BIOS_SHIFT 0x0417                  /* 0040:0017 shift flags (linear) */

typedef u16 (*KeyFn)(void);

static bool vec_is(u16 ivt, u16 off)
{
    return rd16(0, ivt) == off && rd16(0, (u16)(ivt + 2)) == ASM_SEG;
}

/* Host scan-code handler = IRQ 1 */
static void kbd_host_scan(u8 xt_code)
{
    if (vec_is(IVT9, INT9_OFF)) kbd_int9_isr(xt_code);
    /* else: the BIOS handler; the host fills its own BIOS buffer */
}

void input_init(void)
{
    codeptr_register(FN_getkey_raw, (CodeFn)getkey_raw);
    codeptr_register(FN_getkey_menu, (CodeFn)getkey_menu);
    codeptr_register(FN_hotkey_joystick_on, hotkey_joystick_on);
    codeptr_register(FN_hotkey_keyboard_on, hotkey_keyboard_on);
    codeptr_register(FN_hotkey_sound_toggle, hotkey_sound_toggle);
    codeptr_register(FN_hotkey_music_toggle, hotkey_music_toggle);
    codeptr_register(FN_hotkey_pause, hotkey_pause);
    codeptr_register(FN_hotkey_exit, hotkey_exit);
    host_set_scan_handler(kbd_host_scan);
}

/* ================================================================================ INT 9 / INT 16h */

/* 06c9:6860 kbd_install */
void kbd_install(void)
{
    /* PORT: PIC mask writes dropped. Only the INT 9 offset word is compared. */
    if (rd16(0, IVT9) != INT9_OFF) {
        DSW(DS_old_int9) = rd16(0, IVT9);
        DSW(DS_old_int9 + 2) = rd16(0, IVT9 + 2);
        wr16(0, IVT9, INT9_OFF);
        wr16(0, IVT9 + 2, ASM_SEG);
        DSW(DS_old_int16) = rd16(0, IVT16);
        DSW(DS_old_int16 + 2) = rd16(0, IVT16 + 2);
        wr16(0, IVT16, INT16_OFF);
        wr16(0, IVT16 + 2, ASM_SEG);
    }
    for (u16 i = 0; i < 0x5A; i++) DSB((u16)(DS_key_down + i)) = 0;
}

/* 06c9:68c4 kbd_restore */
void kbd_restore(void)
{
    if (DSW(DS_old_int9) == 0) return;          /* offset word only */
    wr16(0, IVT9, DSW(DS_old_int9));
    wr16(0, IVT9 + 2, DSW(DS_old_int9 + 2));
    wr16(0, IVT16, DSW(DS_old_int16));
    wr16(0, IVT16 + 2, DSW(DS_old_int16 + 2));
    mem[BIOS_SHIFT] &= 0xF0;
}

/* 06c9:6907 kbd_int9_isr (port 60h value; the XT acknowledge and the EOI are dropped) */
void kbd_int9_isr(u8 xt_code)
{
    if (xt_code & 0x80) {                      /* break code */
        u16 bx = xt_code & 0x7F;
        if (bx >= 0x5A) bx = 0;
        DSB((u16)(DS_key_down + bx)) = 0;
        return;
    }
    u16 bx = xt_code;
    if (bx >= 0x5A) bx = 0;
    DSW(DS_kbd_last_scan) = bx;
    DSB((u16)(DS_key_down + bx)) = 1;
    u8 al;
    if (DSB(DS_key_down + 0x38) & 1)      al = DSB((u16)(DS_kbd_xlat_alt + bx));
    else if (DSB(DS_key_down + 0x1D) & 1) al = DSB((u16)(DS_kbd_xlat_ctrl + bx));
    else if ((DSB(DS_key_down + 0x2A) & 1) || (DSB(DS_key_down + 0x36) & 1))
                                          al = DSB((u16)(DS_kbd_xlat_shift + bx));
    else if (DSB(DS_key_down + 0x3A) & 1) al = DSB((u16)(DS_kbd_xlat_caps + bx));
    else                                  al = DSB((u16)(DS_kbd_xlat_normal + bx));
    if (!(al & 0x80)) {
        DSW(DS_kbd_last_key) = al;             /* ASCII, AH = 0 (a 0 entry clears the pending key) */
    } else {
        u8 ah = al;
        if (ah >= 0x85) ah &= 0x7F;
        DSW(DS_kbd_last_key) = (u16)(ah << 8); /* extended, AL = 0 */
    }
}

/* 06c9:69b7 kbd_int16_isr */
u16 kbd_int16_isr(u16 ax, bool *zf)
{
    switch (ax >> 8) {
    case 0: {
        u16 k = DSW(DS_kbd_last_key);          /* does not block */
        DSW(DS_kbd_last_key) = 0;
        return k;
    }
    case 1: {
        u16 k = DSW(DS_kbd_last_key);
        if (zf) *zf = k == 0;
        return k;
    }
    case 2:                                    /* AH stays 2, AL = either Shift held */
        return (u16)(ax & 0xFF00) | (u8)(DSB(DS_key_down + 0x2A) | DSB(DS_key_down + 0x36));
    default:
        return 0;
    }
}

/* int 16h AH=01h */
bool kbd_peek(u16 *key)
{
    if (vec_is(IVT16, INT16_OFF)) {
        bool zf = true;
        u16 k = kbd_int16_isr(0x0100, &zf);
        if (key) *key = k;
        return !zf;
    }
    return host_kbd_peek(key);                 /* BIOS */
}

/* int 16h AH=00h */
u16 kbd_read(void)
{
    if (vec_is(IVT16, INT16_OFF)) return kbd_int16_isr(0x0000, NULL);
    u16 k = 0;
    while (!host_kbd_read(&k)) host_pump();    /* the BIOS call waits for a key */
    return k;
}

/* 06c9:69f2 key_is_down (no callers) */
u8 key_is_down(u16 scan)
{
    return DSB((u16)(DS_key_down + scan));
}

/* ================================================================================ hotkeys */

/* 06c9:6528 kbd_dispatch */
u16 kbd_dispatch(u16 key)
{
    if (DSB(DS_hotkey_busy) != 0) return key;
    DSB(DS_hotkey_busy) = 1;
    u16 arg = key, h;
    if ((u8)key != 0) {
        arg = key & 0x7F;                      /* the argument itself is masked: ASCII returns with AH = 0 */
        h = DSW((u16)(DS_hotkey_table + (u16)(arg << 1)));
    } else {
        u16 bx = key >> 8;
        if ((s16)bx >= 0x84) bx = 0x84;
        h = DSW((u16)(DS_key_handlers_ext + (u16)(bx << 1)));
    }
    if (h != 0) {
        CodeFn fn = codeptr_lookup(far_make(ASM_SEG, h));   /* push cs ; call bx (handler ends with retf) */
        fn();
        DSB(DS_hotkey_busy) = 0;
        return 0;
    }
    DSB(DS_hotkey_busy) = 0;
    return arg;
}

/* 06c9:6585 hotkey_set */
void hotkey_set(u16 key, u16 off, u16 seg_unused)
{
    if ((u8)key != 0) {
        if ((s16)key <= 0x7F) DSW((u16)(DS_hotkey_table + (u16)(key << 1))) = off;
        return;
    }
    /* Extended keys: the original loads DS:6148[0x82] into BX and returns without storing anything. */
}

/* 06c9:645a Ctrl-J */
void hotkey_joystick_on(void)
{
    DSB(DS_joy_enabled) = 1;
    DSB(DS_timer_paused) = 1;
    joy_calibrate_screen();
    DSB(DS_timer_paused) = 0;
}

/* 06c9:646f Ctrl-K */
void hotkey_keyboard_on(void)
{
    DSB(DS_joy_enabled) = 0;
    status_strip(0x603C, 8);                   /* "KEYBOARD ON" */
}

/* 06c9:6488 Ctrl-S */
void hotkey_sound_toggle(void)
{
    if (!(DSB(DS_snd_enable) & 1)) {
        DSB(DS_snd_enable) |= 1;
        status_strip(0x6029, 8);               /* "SOUND ON" */
        return;
    }
    DSB(DS_snd_enable) &= 2;
    DSW(DS_sfx_seg) = 0;
    DSB(DS_snd_busy) = 0;
    status_strip(0x6032, 8);                   /* "SOUND OFF" */
}

/* 06c9:64c7 Ctrl-Q */
void hotkey_music_toggle(void)
{
    if (!(DSB(DS_snd_enable) & 2)) {
        DSB(DS_snd_enable) |= 2;
        status_strip(0x6016, 8);               /* "MUSIC ON" */
        return;
    }
    DSB(DS_snd_enable) &= 1;
    DSW(DS_sfx_seg) = 0;
    DSB(DS_snd_busy) = 0;
    status_strip(0x601F, 8);                   /* "MUSIC OFF" */
}

/* 06c9:6508 Ctrl-P */
void hotkey_pause(void)
{
    DSB(DS_timer_paused) = 1;
    pause_prompt();
    DSB(DS_timer_paused) = 0;
}

/* 06c9:6518 Ctrl-X */
void hotkey_exit(void)
{
    DSB(DS_timer_paused) = 1;
    exit_prompt();
    DSB(DS_timer_paused) = 0;
}

/* 06c9:5d1e wait_key_paused (no direct callers) */
void wait_key_paused(void)
{
    DSB(DS_timer_paused) = 1;
    getkey_wait();
    DSB(DS_timer_paused) = 0;
}

/* ================================================================================ getkey family */

/* 06c9:69fe getkey: far call through DS:64DC */
u16 getkey(void)
{
    KeyFn fn = (KeyFn)codeptr_lookup(ds_far(DS_getkey_fn));
    return fn();
}

/* 06c9:6a03 getkey_raw */
u16 getkey_raw(void)
{
    if (!kbd_peek(NULL)) return 0;
    u16 k = kbd_read();
    if ((u8)k != 0) k &= 0x00FF;
    return k;
}

/* 06c9:65c5 getkey_menu */
u16 getkey_menu(void)
{
    if (kbd_peek(NULL)) return kbd_dispatch(kbd_read());
    u16 j = joy_read();
    u16 r = (j & 0x30) ? 0x000D : DSW((u16)(DS_joy_menu_keys + ((j & 0x0F) << 1)));
    if (r == DSW(DS_joy_menu_last)) return 0;
    DSW(DS_joy_menu_last) = r;
    return r;
}

/* 06c9:6601 getkey_drive */
u16 getkey_drive(void)
{
    if (!kbd_peek(NULL)) return 0;
    /* PORT: the "SS != DS -> return the peeked key without reading it" case (the driving tick
     * interrupting code on a foreign stack) cannot happen in the port. */
    return kbd_dispatch(kbd_read());
}

/* 06c9:6a17 */
void kbd_set_getkey_fn(FarPtr fn)
{
    ds_far_wr(DS_getkey_fn, fn);
}

/* 06c9:6a28 */
FarPtr kbd_get_getkey_fn(void)
{
    return ds_far(DS_getkey_fn);
}

/* 06c9:6a30 getkey_wait */
u16 getkey_wait(void)
{
    u16 k;
    while ((k = getkey()) == 0) host_pump();
    return k;
}

/* 06c9:6a3b kbd_flush (no hotkeys) */
u16 kbd_flush(void)
{
    while (kbd_peek(NULL)) kbd_read();
    return 0;
}

/* 06c9:6a4a getkey_until_deadline */
u16 getkey_until_deadline(void)
{
    for (;;) {
        u16 k = getkey();
        if (k) return k;
        if (ticks_get() >= DSL(DS_deadline)) return 0;
        host_pump();
    }
}

/* 06c9:6a6a getkey_timeout */
u16 getkey_timeout(u32 n)
{
    u32 end = ticks_get() + n;
    for (;;) {
        u16 k = getkey();
        if (k) return k;
        if (ticks_get() >= end) return 0;
        host_pump();
    }
}

/* 06c9:c63e getkey_until_slow_deadline. The original compares "hi < dhi || lo < dlo" (see timer.c);
 * PORT: compared as u32. */
u16 getkey_until_slow_deadline(void)
{
    for (;;) {
        u16 k = getkey();
        if (k) return k;
        if (slow_ticks_get() >= DSL(DS_slow_deadline)) return 0;
        host_pump();
    }
}

/* 06c9:c65c wait_key_slow (no callers; same compare) */
u16 wait_key_slow(u32 n)
{
    u32 end = slow_ticks_get() + n;
    for (;;) {
        u16 k = getkey();
        if (k) return k;
        if (slow_ticks_get() >= end) return 0;
        host_pump();
    }
}

/* ================================================================================ driving input, joystick */

/* 06c9:6620 input_drive_bits */
u16 input_drive_bits(void)
{
    u16 r = 0;
    if (DSB(DS_key_down + 0x39)) r |= 0x10;    /* Space -> button A */
    if (DSB(DS_key_down + 0x1C)) r |= 0x20;    /* Enter -> button B */
    if (DSB(DS_key_down + 0x47)) r |= 0x09;    /* Home  -> up + left */
    if (DSB(DS_key_down + 0x48)) r |= 0x01;    /* Up */
    if (DSB(DS_key_down + 0x49)) r |= 0x05;    /* PgUp  -> up + right */
    if (DSB(DS_key_down + 0x4D)) r |= 0x04;    /* Right */
    if (DSB(DS_key_down + 0x51)) r |= 0x06;    /* PgDn  -> down + right */
    if (DSB(DS_key_down + 0x50)) r |= 0x02;    /* Down */
    if (DSB(DS_key_down + 0x4F)) r |= 0x0A;    /* End   -> down + left */
    if (DSB(DS_key_down + 0x4B)) r |= 0x08;    /* Left */
    if (r == 0) r = joy_read();
    return r;
}

/* 06c9:6686 joy_read */
u16 joy_read(void)
{
    if (DSB(DS_joy_enabled) == 0) return 0;
    if (!(DSW(DS_joy_calibrated) & 1)) return 0;
    DSB(DS_joy_result) = 0;
    /* PORT: the port 201h one-shot timing (4000-iteration timeout) and the adaptive min/max calibration
     * (DS:6282..629D) are replaced by fixed thresholds on the host gamepad (platform.md §6, §9.2 q5):
     * x < -16384 left (8), x >= 16384 right (4), y < -16384 up (1), y >= 16384 down (2); South = button A
     * (0x10), East = button B (0x20). joy_x / joy_y get synthetic counts (centre 0x50) for the unused
     * analog helpers. No gamepad: no bits, like a missing stick. */
    s16 x, y;
    u8 buttons;
    if (host_joy_read(&x, &y, &buttons)) {
        u8 r = 0;
        if (x < -16384) r |= 8;
        else if (x >= 16384) r |= 4;
        if (y < -16384) r |= 1;
        else if (y >= 16384) r |= 2;
        if (buttons & 1) r |= 0x10;
        if (buttons & 2) r |= 0x20;
        DSB(DS_joy_result) = r;
        DSW(DS_joy_x) = (u16)(0x50 + (x >> 10));
        DSW(DS_joy_y) = (u16)(0x50 + (y >> 10));
    } else {
        DSW(DS_joy_x) = 0x50;
        DSW(DS_joy_y) = 0x50;
    }
    return DSB(DS_joy_result);
}

/* 06c9:680a joy_calib_reset (the over-counters are not reset) */
void joy_calib_reset(void)
{
    DSB(DS_joy_enabled) = 1;
    DSW(DS_joy_xmin) = 0x50;
    DSW(DS_joy_xmax) = 0;
    DSW(DS_joy_ymin) = 0x50;
    DSW(DS_joy_ymax) = 0;
}

/* 06c9:6828 joy_dir_index: 0 centre, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SW, 7 W, 8 NW */
u16 joy_dir_index(u16 bits)
{
    return DSB((u16)(DS_joy_dir_map + (bits & 0x0F)));
}

/* 06c9:6839 joy_analog_x (no callers) */
s16 joy_analog_x(void)
{
    u32 p = (u32)(u16)(DSW(DS_joy_x) - DSW(DS_joy_xmin)) * DSW(DS_joy_xscale);
    return (s16)((u16)(p >> 8) - 0x1F);
}

/* 06c9:684c joy_analog_y (no callers) */
s16 joy_analog_y(void)
{
    u32 p = (u32)(u16)(DSW(DS_joy_y) - DSW(DS_joy_ymin)) * DSW(DS_joy_yscale);
    return (s16)((u16)(p >> 8) - 0x1F);
}

/* ================================================================================ C helpers */

/* 16aa:0002 toupper_c */
s16 toupper_c(s16 c)
{
    s8 b = (s8)c;
    if (b >= 'a' && b <= 'z') b = (s8)(b - 0x20);
    return b;
}

/* ================================================================================ 16bb: text-line editor */

/* 16bb:02e6 edit_cursor_xor: redraws (XOR) the cursor if it is currently shown */
void edit_cursor_xor(void)
{
    if (DSW(DS_g_edit_cursor_on) == 0) return;
    draw_cursor_glyph((s16)(DSW(DS_g_edit_x) + (u16)(DSW(DS_g_edit_pos) << 3)), (s16)DSW(DS_g_edit_y),
                      DSW(DS_g_edit_cursor_h));
}

/* 16bb:000a edit_text_line */
s16 edit_text_line(u16 buf_ds, s16 maxlen, s16 x, s16 y, u32 timeout)
{
    DSW(DS_g_edit_x) = (u16)x;
    DSW(DS_g_edit_y) = (u16)y;
    DSB((u16)(buf_ds + maxlen)) = 0;
    s16 i = 0;
    while (DSB((u16)(buf_ds + i)) != 0) i++;
    for (; i < maxlen; i++) DSB((u16)(buf_ds + i)) = ' ';
    gfx_draw_text(buf_ds, (u16)x, (u16)y);
    DSW(DS_g_edit_pos) = 0;
    DSW(DS_g_edit_cursor_h) = 1;
    s16 insert = 0;
    draw_cursor_glyph(x, y, 1);
    DSW(DS_g_edit_cursor_on) = 1;
    deadline_set(timeout);
    slow_deadline_set(4);

    s16 key;
    for (;;) {
        key = (s16)getkey_until_slow_deadline();
        if (key == 0) {                                     /* blink every 4 slow ticks */
            slow_deadline_set(4);
            draw_cursor_glyph((s16)(x + (s16)(DSW(DS_g_edit_pos) << 3)), y, DSW(DS_g_edit_cursor_h));
            DSW(DS_g_edit_cursor_on) = DSW(DS_g_edit_cursor_on) ? 0 : 1;
            if (timeout == 0) continue;
            if (!deadline_passed()) continue;
            break;                                          /* timeout: key = 0 */
        }
        deadline_set(timeout);
        if (key == 0x0D || key == 0x1B || (u16)key == 0x4800 || (u16)key == 0x5000 || key == 0x09) break;
        s16 pos = (s16)DSW(DS_g_edit_pos);
        if ((u16)key == 0x4D00) {                           /* Right: may reach maxlen */
            edit_cursor_xor();
            if (maxlen > (s16)DSW(DS_g_edit_pos)) DSW(DS_g_edit_pos)++;
        } else if ((u16)key == 0x4B00) {                    /* Left */
            edit_cursor_xor();
            if (DSW(DS_g_edit_pos) != 0) DSW(DS_g_edit_pos)--;
        } else if ((u16)key == 0x4700) {                    /* Home */
            edit_cursor_xor();
            DSW(DS_g_edit_pos) = 0;
        } else if ((u16)key == 0x4F00) {                    /* End */
            edit_cursor_xor();
            DSW(DS_g_edit_pos) = (u16)maxlen;
        } else if ((u16)key == 0x5200) {                    /* Ins */
            edit_cursor_xor();
            if (insert == 0) { insert = 1; DSW(DS_g_edit_cursor_h) = 8; }
            else             { insert = 0; DSW(DS_g_edit_cursor_h) = 1; }
        } else if ((u16)key == 0x5300) {                    /* Del (the cursor is not removed first) */
            if (!(maxlen > pos)) continue;
            for (i = pos; (s16)(maxlen - 1) > i; i++)
                DSB((u16)(buf_ds + i)) = DSB((u16)(buf_ds + i + 1));
            DSB((u16)(buf_ds + maxlen - 1)) = ' ';
            gfx_draw_text(buf_ds, (u16)x, (u16)y);
        } else if (key == 0x08) {                           /* Backspace */
            if (pos == 0) continue;
            edit_cursor_xor();
            DSW(DS_g_edit_pos)--;
            DSB((u16)(buf_ds + DSW(DS_g_edit_pos))) = ' ';
            gfx_draw_text(buf_ds, (u16)x, (u16)y);
        } else {
            if (key < 0x20 || key > 0x7A) continue;         /* signed compares: extended keys are ignored */
            if (!(maxlen > pos)) continue;
            edit_cursor_xor();
            if (insert)
                for (i = (s16)(maxlen - 2); i >= (s16)DSW(DS_g_edit_pos); i--)
                    DSB((u16)(buf_ds + i + 1)) = DSB((u16)(buf_ds + i));
            pos = (s16)DSW(DS_g_edit_pos);
            DSB((u16)(buf_ds + pos)) = (u8)key;
            if (maxlen > pos) DSW(DS_g_edit_pos)++;
            gfx_draw_text(buf_ds, (u16)x, (u16)y);
        }
        edit_cursor_xor();                                  /* 16bb:023f */
    }
    edit_cursor_xor();                                      /* remove the cursor */
    return key;
}

/* 16bb:02a8 input_text_line */
s16 input_text_line(u16 buf_ds, s16 maxlen, s16 x, s16 y, u32 timeout)
{
    for (s16 i = 0; i < maxlen; i++) DSB((u16)(buf_ds + i)) = ' ';
    return edit_text_line(buf_ds, maxlen, x, y, timeout);
}
