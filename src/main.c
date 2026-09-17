/* Test Drive II: The Duel SDL3 port — entry point.
 *
 * usage: td2port [--game-dir DIR] [--scale N] [--frame-rate FPS] [--check]
 *   --game-dir   folder with the original game files (default: "Game" in the working directory)
 *   --scale      initial window scale (default 3)
 *   --frame-rate emulated original drawing speed while driving (default HOST_DEFAULT_FPS; 0 = unpaced)
 *   --check      load and verify the original executable, print a summary and exit (no window)
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "mem.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/timer.h"

int game_main(void);   /* game/flow.c: port of main() at 0000:07b3 */

int main(int argc, char **argv)
{
    const char *dir = "Game";
    int scale = 3;
    bool check = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--frame-rate") && i + 1 < argc) host_set_frame_rate(atoi(argv[++i]));
        else {
            fprintf(stderr, "usage: %s [--game-dir DIR] [--scale N] [--frame-rate FPS] [--check]\n", argv[0]);
            return 2;
        }
    }

    char exe_path[1024];
    snprintf(exe_path, sizeof exe_path, "%s/%s", dir, TD_EXE_NAME);
    char err[256];
    if (!mem_load_exe(exe_path, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Test Drive II", err, NULL);
        return 1;
    }
    if (check) {
        printf("%s ok (%s): image %u bytes at %04X:0000, DGROUP %04X\n", TD_EXE_NAME, TD_VARIANT_NAME,
               mem_image_size, LOAD_SEG, DGROUP);
        return 0;
    }

    if (!host_init(dir, scale)) return 1;
    gfx_init();      /* EGA model, frame source */
    timer_init();    /* host tick handler, timer routines */
    input_init();    /* INT 9 handler, getkey code pointers */
    int rc = game_main();
    host_shutdown();
    return rc;
}
