#pragma once
/* Host services on top of SDL3: window/present, the 99.9985 Hz timer tick, keyboard (raw XT scan codes
 * for the game's own INT 9 handler, plus a BIOS-style buffer), gamepad, PC speaker audio, game file
 * lookup and fatal errors. No game logic lives here. */
#include "types.h"

#define PIT_HZ        1193182u
#define PIT_DIV_GAME  0x2E9Cu        /* 11932 -> 99.9985 Hz, the game's timer rate (06c9:6059) */

bool host_init(const char *game_dir, int window_scale);
void host_shutdown(void);

/* Called once per 99.9985 Hz tick from host_pump() (the port's replacement for INT 8).
 * The timer module installs its ISR body (tick counters and the timer routine list). */
void host_set_tick_handler(void (*handler)(void));

/* Source of the displayed image: fills a w x h XRGB8888 frame (at most HOST_FRAME_MAX_W x
 * HOST_FRAME_MAX_H) and returns true if it changed since the last call. The frame is shown with 4:3
 * aspect. Installed by the graphics module (EGA/CGA 320x200, Hercules 640x300). */
#define HOST_FRAME_MAX_W 640
#define HOST_FRAME_MAX_H 300
void host_set_frame_source(bool (*compose)(u32 *xrgb), int w, int h);

/* Runs due timer ticks, generates speaker audio, handles window events and presents the screen when
 * it changed. Every busy-wait loop of the original (key polls, deadlines, delays) must call this.
 * Sleeps briefly when nothing was due, so tight polling loops do not spin the CPU. */
void host_pump(void);

/* Emulated original frame rate (port option, default HOST_DEFAULT_FPS). The original's stage loop ran as
 * fast as the PC could draw; the simulation runs on timer ticks, but a few things count rendered frames
 * (gear-gate close delay, dash phase). host_frame_begin() pumps and then waits for the next frame slot at
 * the configured rate (0 = unpaced). */
#define HOST_DEFAULT_FPS 15
void host_set_frame_rate(int fps);
int  host_frame_rate(void);
void host_frame_begin(void);

/* Presents immediately if the frame source reports a change (used by unpaced effects that the
 * original drew at CPU speed). Blocks on VSync. */
void host_present_now(void);

/* ---- Raw keyboard (the game's INT 9 handler, 06c9:6907). The handler is called from host_pump() /
 * host event processing, in event order, with the XT set-1 scan code of each key event: make codes for
 * presses and key repeats (typematic), code | 0x80 for releases. Extended keys (arrows, Home, ...) give
 * the same codes as their keypad twins, without the E0 prefix. F11/F12 are not reported. */
void host_set_scan_handler(void (*handler)(u8 xt_code));

/* ---- BIOS keyboard (INT 16h; not used by TD2, which reads its own INT 9 state). Key words are (XT scan code << 8) | ASCII, including key repeats. */
bool host_kbd_peek(u16 *key);    /* AH=01h: true if a key is buffered (not removed) */
bool host_kbd_read(u16 *key);    /* AH=00h without blocking: removes and returns the oldest key */
void host_kbd_flush(void);
u8   host_kbd_shift_flags(void); /* AH=02h: bit0 right shift, bit1 left shift, bit2 ctrl, bit3 alt */

/* Held-key driving controls (port option, default on): the driving poll reads directions and the
 * A/Z shift keys from the live keyboard state instead of BIOS key repeats. --bios-keys turns it off. */
void host_set_held_keys(bool on);
bool host_held_keys(void);
/* True while the key with this XT scan code is down. Supports A (0x1E), Z (0x2C) and the cursor /
 * keypad block 0x47..0x51 (arrows and keypad digits both count). */
bool host_xt_key_down(u8 xt_scan);

/* ---- Joystick: first connected gamepad. Axes -32768..32767, buttons bit0 = A, bit1 = B. */
bool host_joy_read(s16 *x, s16 *y, u8 *buttons);

/* ---- PC speaker: PIT channel 2 divisor and the port 61h gate (bits 0 and 1 both set = sounding).
 * divisor 0 means 65536. Changes take effect from the current tick onward. */
void host_speaker(u16 divisor, bool on);

/* ---- Game files: case-insensitive lookup inside the game directory. Returns a malloc'd path
 * (SDL_free it) or NULL if the file does not exist. For new files (SCORES) pass create = true. */
char *host_game_path(const char *name, bool create);
void  host_free(void *p);        /* frees memory returned by host functions (host_game_path) */

/* ---- Errors: shows a message box, shuts down and exits with code 3 (like the original abort). */
_Noreturn void host_fatal(const char *fmt, ...);
