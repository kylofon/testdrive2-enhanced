#pragma once
/* Hotkey installation, exit / pause prompts, the status strip (segment 16fc) and the joystick
 * calibration screen (1769:000e) — port/spec/platform.md §4.10, §4.11; game_flow.md §4.13.
 * They draw through platform/gfx.h over a saved screen band and keep pumping the host while waiting. */
#include "../mem.h"

void hotkeys_install(void);                 /* 16fc:0002 Ctrl-J/K/P/Q/S/X */
void exit_prompt(void);                     /* 16fc:007c "EXIT TO DOS (Y/N)"; Y exits the program (code 0) */
void status_strip(u16 msg_ds, u32 ticks);   /* 16fc:0220 message bar at y = 190 for `ticks` slow ticks */
void pause_prompt(void);                    /* 16fc:0358 "PAUSE - PRESS ANY KEY TO RESUME" */
void joy_calibrate_screen(void);            /* 1769:000e */
