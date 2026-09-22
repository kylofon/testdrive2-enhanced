/* Test Drive II Enhanced — entry point.
 *
 * usage: testdrive2-enhanced [--game-dir DIR] [--scale N] [--res-scale N] [--draw-distance N]
 *                            [--frame-rate FPS] [--sprite-detail max|auto] [--valley on|off]
 *                            [--show-position on|off] [--classic] [--check]
 *                            [--viewer STAGE [--viewer-start UNIT]]
 *   --game-dir      folder with the original game files (default: "Game" in the working directory)
 *   --scale         initial window scale (default 3)
 *   --res-scale     ENH: output resolution as a multiple of 320x200 (default 4, 1..8)
 *   --draw-distance ENH: road units drawn by the enhanced renderer (default 180, 60..240)
 *   --frame-rate    drawing rate while driving (default HOST_DEFAULT_FPS = 60; 0 = unpaced)
 *   --sprite-detail ENH: max (default): the most detailed sprite of every car and object at every distance,
 *                   scaled to its size; auto: the size variant chosen by distance
 *   --valley        ENH: on: a valley floor far below drop-offs; off (default): the sky below them, as in the original
 *   --show-position ENH: on: stage code, road unit and lateral in the corner of the road view (F9 toggles)
 *                   (as TD2_ENH_STAGE / TD2_ENH_START take them); F9 toggles it
 *   --classic       ENH: original renderer at the original 15 fps (for comparison)
 *   --check         load and verify the original executable, print a summary and exit (no window)
 *   --viewer        ENH: map viewer: fly along a stage (e.g. CCC0, TDS21, EC_5: scenery code and stage digit)
 *                   with the enhanced road view filling the window, no game (enhanced/enh_viewer.c)
 *   --viewer-start  ENH: the road unit the viewer starts at (default 0)
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "mem.h"
#include "enhanced/enhanced.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/timer.h"

int game_main(void);   /* game/flow.c: port of main() at 0000:07b3 */

static const char USAGE[] = "usage: %s [--game-dir DIR] [--scale N] [--res-scale N] [--draw-distance N] "
                            "[--frame-rate FPS] [--sprite-detail max|auto] [--valley on|off] [--show-position on|off] "
                            "[--classic] [--check] [--viewer STAGE [--viewer-start UNIT]]\n";

int main(int argc, char **argv)
{
    const char *dir = "Game";
    int scale = 3, res_scale = 4, draw_distance = ENH_DEFAULT_ROWS, frame_rate = -1;
    bool check = false, classic = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--res-scale") && i + 1 < argc) res_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--draw-distance") && i + 1 < argc) draw_distance = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frame-rate") && i + 1 < argc) frame_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sprite-detail") && i + 1 < argc && !strcmp(argv[i + 1], "max")) { enh_detail_max = true; i++; }
        else if (!strcmp(argv[i], "--sprite-detail") && i + 1 < argc && !strcmp(argv[i + 1], "auto")) { enh_detail_max = false; i++; }
        else if (!strcmp(argv[i], "--valley") && i + 1 < argc && !strcmp(argv[i + 1], "on")) { enh_valley = true; i++; }
        else if (!strcmp(argv[i], "--valley") && i + 1 < argc && !strcmp(argv[i + 1], "off")) { enh_valley = false; i++; }
        else if (!strcmp(argv[i], "--show-position") && i + 1 < argc && !strcmp(argv[i + 1], "on")) { enh_show_position = true; i++; }
        else if (!strcmp(argv[i], "--show-position") && i + 1 < argc && !strcmp(argv[i + 1], "off")) { enh_show_position = false; i++; }
        else if (!strcmp(argv[i], "--classic")) classic = true;
        else if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--viewer") && i + 1 < argc) enh_viewer_stage = argv[++i];
        else if (!strcmp(argv[i], "--viewer-start") && i + 1 < argc) enh_viewer_start = atoi(argv[++i]);
        else {
            fprintf(stderr, USAGE, argv[0]);
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

    if (enh_viewer_stage) classic = false;           /* ENH: the viewer is the enhanced renderer */
    if (!host_init(dir, scale)) return 1;
    /* ENH: output scale before the frame source is installed; the classic mode shows the plain EGA frame */
    gfx_set_output_scale(classic ? 1 : res_scale);
    host_set_frame_rate(frame_rate >= 0 ? frame_rate : classic ? HOST_ORIGINAL_FPS : HOST_DEFAULT_FPS);
    gfx_init();      /* EGA model, frame source */
    timer_init();    /* host tick handler, timer routines */
    input_init();    /* INT 9 handler, getkey code pointers */
    enh_init(!classic, draw_distance);   /* ENH: overlay (off in the classic mode) */
    int rc = game_main();
    host_shutdown();
    return rc;
}
