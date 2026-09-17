/* Hotkey installation, exit / pause prompts and the status strip (segment 16fc), joystick calibration
 * screen (1769:000e) — port/spec/platform.md §4.10, §4.11; game_flow.md §4.13; checked against the
 * disassembly.
 *
 * The original keeps the 26-word target state and the 11-word text state in stack locals (DGROUP) and
 * passes their addresses; the port takes them from the DGROUP stack region (ds_stack_alloc). All waits
 * pump the host. */
#include "prompts.h"

#include <stdlib.h>

#include "../codeptr.h"
#include "../host.h"
#include "../symbols.h"
#include "gfx.h"
#include "input.h"
#include "res.h"
#include "timer.h"

#define DS_calib_line1 0x6C6E   /* " Calibrate your joystick by using it " */
#define DS_calib_line2 0x6C94   /* " to move the joystick position indicator " */
#define DS_calib_line3 0x6CBE   /* " below, to all squares " */
#define DS_calib_line4 0x6CD6   /* " Press joystick button when complete " */
#define CS_screen_stride 0xAF68 /* screen descriptor + 0x14 */

/* 16fc:0002 hotkeys_install */
void hotkeys_install(void)
{
    hotkey_set(0x0A, 0x645A, ASM_SEG);        /* Ctrl-J hotkey_joystick_on */
    hotkey_set(0x0B, 0x646F, ASM_SEG);        /* Ctrl-K hotkey_keyboard_on */
    hotkey_set(0x10, 0x6508, ASM_SEG);        /* Ctrl-P hotkey_pause */
    hotkey_set(0x11, 0x64C7, ASM_SEG);        /* Ctrl-Q hotkey_music_toggle */
    hotkey_set(0x13, 0x6488, ASM_SEG);        /* Ctrl-S hotkey_sound_toggle */
    hotkey_set(0x18, 0x6518, ASM_SEG);        /* Ctrl-X hotkey_exit */
}

/* Common frame of the three boxes: save buffer, canvas, saved states */
typedef struct {
    FarPtr save, canvas;
    u16 gstate, tstate;                       /* DGROUP locals */
} Box;

static void box_open(Box *b, s16 sx, s16 sy, u16 w, u16 h)
{
    b->save = gfx_create_buffer(w, h, 0x0F);
    b->canvas = gfx_create_buffer(w, h, 0x0F);
    b->gstate = ds_stack_alloc(GFX_SAVE_WORDS * 2);
    b->tstate = ds_stack_alloc(TEXT_STATE_WORDS * 2);
    gfx_targets_save(b->gstate);
    text_state_save(b->tstate);
    gfx_select_target(b->save);
    gfx_grab_screen(sx, sy, 0, 0, (s16)w, (s16)h);
    gfx_select_target(b->canvas);
    gfx_clear_clip(0);
}

static void box_show(Box *b, s16 sx, s16 sy)
{
    gfx_select_target(gfx_screen_desc());
    blit_copy_raw(gfx_desc_sprite(b->canvas), sx, sy);
}

static void box_close(Box *b, s16 sx, s16 sy)
{
    blit_copy_raw(gfx_desc_sprite(b->save), sx, sy);
    text_state_restore(b->tstate);
    gfx_targets_restore(b->gstate);
    gfx_free_buffer(b->canvas);
    gfx_free_buffer(b->save);
    ds_stack_release(b->gstate);              /* frees tstate too */
}

/* 16fc:007c exit_prompt */
void exit_prompt(void)
{
    Box b;
    box_open(&b, 0x50, 0x58, 0xA0, 0x18);
    draw_rect_outline(4, 4, 0x9B, 0x14, 4);
    gfx_set_text_colours(15, 0);
    draw_text_centered(DS_str_exit_to_dos, 9);          /* "EXIT TO DOS (Y/N)" */
    box_show(&b, 0x50, 0x58);
    FarPtr fn = kbd_get_getkey_fn();
    kbd_set_getkey_fn(codeptr_far(FN_getkey_raw));
    u16 k = getkey_wait();
    if (toupper_c((s16)k) == 'Y') {
        kbd_restore();
        timer_restore();
        gfx_shutdown();
        /* PORT: exit(0) after the hardware restore -> close the host (window, audio) and exit. */
        host_shutdown();
        exit(0);
    }
    kbd_set_getkey_fn(fn);
    box_close(&b, 0x50, 0x58);
}

/* 16fc:0220 status_strip */
void status_strip(u16 msg_ds, u32 ticks)
{
    Box b;
    box_open(&b, 0, 0xBE, 0x140, 0x0A);
    gfx_set_text_colours(15, 0);
    draw_text_centered(msg_ds, 1);
    box_show(&b, 0, 0xBE);
    delay_slow(ticks);                        /* not paused: the game keeps running meanwhile */
    box_close(&b, 0, 0xBE);
}

/* 16fc:0358 pause_prompt */
void pause_prompt(void)
{
    Box b;
    box_open(&b, 0, 0x58, 0x140, 0x18);
    gfx_set_text_colours(15, 0);
    draw_rect_outline(4, 4, 0x13C, 0x14, 4);
    draw_text_centered(DS_str_pause, 8);                 /* "PAUSE - PRESS ANY KEY TO RESUME" */
    box_show(&b, 0, 0x58);
    FarPtr fn = kbd_get_getkey_fn();
    kbd_set_getkey_fn(codeptr_far(FN_getkey_raw));         /* keyboard only, no hotkeys */
    while (getkey_wait() == 0) host_pump();
    kbd_set_getkey_fn(fn);
    box_close(&b, 0, 0x58);
}

/* 1769:000e joy_calibrate_screen */
void joy_calibrate_screen(void)
{
    DSW(DS_joy_calibrated) = 1;
    if (input_drive_bits() & 0x10) {          /* Space held or button A: cancel */
        DSW(DS_joy_enabled) = 0;              /* word writes */
        DSW(DS_joy_calibrated) = 0;
        kbd_flush();
        return;
    }
    u16 gstate = ds_stack_alloc(GFX_SAVE_WORDS * 2);
    u16 tstate = ds_stack_alloc(TEXT_STATE_WORDS * 2);
    gfx_targets_save(gstate);
    text_state_save(tstate);
    FarPtr save = gfx_create_buffer(0x140, 0xC8, 0x0F);
    gfx_select_target(save);
    gfx_grab_screen(0, 0, 0, 0, 0x140, 0xC8);
    gfx_clear_screen(0);
    gfx_select_target(gfx_screen_desc());
    gfx_set_clip_current(0, (s16)CSW(CS_screen_stride), 0, 0xC8);
    gfx_set_text_colours(15, 0);
    draw_text_centered(DS_calib_line1, 0x23);
    draw_text_centered(DS_calib_line2, 0x2D);
    draw_text_centered(DS_calib_line3, 0x37);
    draw_text_centered(DS_calib_line4, 0xB9);
    gfx_draw_line_or(0x82, 0x46, 0x82, 0xA0, 15);        /* 06c9:c984 */
    gfx_draw_line_or(0xAA, 0x46, 0xAA, 0xA0, 15);
    gfx_draw_line_or(0x5A, 0x64, 0xD2, 0x64, 15);
    gfx_draw_line_or(0x5A, 0x82, 0xD2, 0x82, 15);
    s16 prev = -1;
    joy_calib_reset();
    for (;;) {                                /* 1769:0176 */
        host_pump();
        if (getkey_raw()) break;              /* any key ends */
        u16 j = joy_read();
        if (j & 0x30) break;                  /* either button ends */
        s16 d = (s16)joy_dir_index(j);
        if (d == prev) continue;
        for (u16 i = 0; i < 9; i++)
            gfx_fill_rect((s16)DSW((u16)(DS_calib_sq_x + (i << 1))), (s16)DSW((u16)(DS_calib_sq_y + (i << 1))),
                          0x20, 0x18, 0);
        gfx_fill_rect((s16)DSW((u16)(DS_calib_sq_x + (d << 1))), (s16)DSW((u16)(DS_calib_sq_y + (d << 1))),
                      0x20, 0x18, 4);
        prev = d;
    }
    blit_copy_own(gfx_desc_sprite(save));
    gfx_free_buffer(save);
    text_state_restore(tstate);
    gfx_targets_restore(gstate);
    ds_stack_release(gstate);
    delay_slow(4);
    kbd_flush();
}
