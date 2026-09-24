/* Test Drive II Enhanced — entry point.
 *
 * usage: testdrive2-enhanced [--game-dir DIR] [--scale N] [--res-scale N] [--draw-distance N]
 *                            [--frame-rate FPS] [--sim-ticks N] [--sprite-detail max|auto] [--valley on|off]
 *                            [--show-position on|off] [--enhanced-road on|off] [--enhanced-sides on|off]
 *                            [--mix-cars on|off]
 *                            [--start STAGE|default] [--race clock|opponent] [--car CODE] [--opponent CODE]
 *                            [--classic] [--check] [--viewer STAGE [--viewer-start UNIT]]
 *   --game-dir      folder with the original game files (default: "Game" in the working directory)
 *   --scale         initial window scale (default 3)
 *   --res-scale     ENH: output resolution as a multiple of 320x200 (default 4, 1..8)
 *   --draw-distance ENH: road units drawn by the enhanced renderer (default 180, 60..240)
 *   --frame-rate    drawing rate while driving (default HOST_DEFAULT_FPS = 60; 0 = unpaced)
 *   --sim-ticks     ENH: game speed, timer ticks per simulation step (default HOST_DEFAULT_SIM_TICKS = 6; the
 *                   original: 10; 3..20); the race clock stays real time
 *   --sprite-detail ENH: max (default): the most detailed sprite of every car and object at every distance,
 *                   scaled to its size; auto: the size variant chosen by distance
 *   --valley        ENH: on: a valley floor far below drop-offs; off (default): the sky below them, as in the original
 *   --show-position ENH: on: stage code, road unit and lateral in the corner of the road view (F9 toggles)
 *                   (as TD2_ENH_STAGE / TD2_ENH_START take them); F9 toggles it
 *   --enhanced-road ENH: on (default): the road and its shoulders alternate between a lighter and a darker shade
 *                   every two road units; off: the original's plain road
 *   --enhanced-sides ENH: on (default): the ground beside the road alternates with them; off: plain
 *   --mix-cars      ENH: on: some traffic drawn as the other sceneries' cars (the Beetle and the Saab in California
 *                   and the Master Scenery, the Mercedes in Europe), at least one of each own car kept; off (default)
 *   --start         straight into a race on that stage (e.g. CCC0; default: the chosen scenery's first stage),
 *                   without the intro, the menus and the difficulty screen; then the game goes on as usual
 *   --race          with --start: clock (default) or opponent
 *   --car, --opponent  the player's / the opponent's car by its CARS.DAT code (e.g. F40), as if chosen in the menu
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
#include "game/flow.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/timer.h"

static const char USAGE[] = "usage: %s [--game-dir DIR] [--scale N] [--res-scale N] [--draw-distance N] "
                            "[--frame-rate FPS] [--sim-ticks N] [--sprite-detail max|auto] [--valley on|off] [--show-position on|off] "
                            "[--enhanced-road on|off] [--enhanced-sides on|off] [--mix-cars on|off] [--start STAGE|default] "
                            "[--race clock|opponent] [--car CODE] [--opponent CODE] "
                            "[--classic] [--check] [--viewer STAGE [--viewer-start UNIT]]\n";

/* "on" / "off" of an option into *v; false for anything else */
static bool on_off(const char *s, bool *v)
{
    if (!strcmp(s, "on")) *v = true;
    else if (!strcmp(s, "off")) *v = false;
    else return false;
    return true;
}

int main(int argc, char **argv)
{
    const char *dir = "Game";
    int scale = 3, res_scale = 4, draw_distance = ENH_DEFAULT_ROWS, frame_rate = -1, sim_ticks = 0;
    bool check = false, classic = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--res-scale") && i + 1 < argc) res_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--draw-distance") && i + 1 < argc) draw_distance = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frame-rate") && i + 1 < argc) frame_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sim-ticks") && i + 1 < argc) sim_ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sprite-detail") && i + 1 < argc && !strcmp(argv[i + 1], "max")) { enh_detail_max = true; i++; }
        else if (!strcmp(argv[i], "--sprite-detail") && i + 1 < argc && !strcmp(argv[i + 1], "auto")) { enh_detail_max = false; i++; }
        else if (!strcmp(argv[i], "--valley") && i + 1 < argc && !strcmp(argv[i + 1], "on")) { enh_valley = true; i++; }
        else if (!strcmp(argv[i], "--valley") && i + 1 < argc && !strcmp(argv[i + 1], "off")) { enh_valley = false; i++; }
        else if (!strcmp(argv[i], "--show-position") && i + 1 < argc && !strcmp(argv[i + 1], "on")) { enh_show_position = true; i++; }
        else if (!strcmp(argv[i], "--show-position") && i + 1 < argc && !strcmp(argv[i + 1], "off")) { enh_show_position = false; i++; }
        else if (!strcmp(argv[i], "--enhanced-road") && i + 1 < argc && on_off(argv[i + 1], &enh_road_bands)) i++;
        else if (!strcmp(argv[i], "--enhanced-sides") && i + 1 < argc && on_off(argv[i + 1], &enh_side_bands)) i++;
        else if (!strcmp(argv[i], "--mix-cars") && i + 1 < argc && on_off(argv[i + 1], &enh_mix_cars)) i++;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) flow_start_stage = argv[++i];
        else if (!strcmp(argv[i], "--race") && i + 1 < argc && !strcmp(argv[i + 1], "clock")) { flow_start_mode = 0; i++; }
        else if (!strcmp(argv[i], "--race") && i + 1 < argc && !strcmp(argv[i + 1], "opponent")) { flow_start_mode = 1; i++; }
        else if (!strcmp(argv[i], "--car") && i + 1 < argc) flow_start_car = argv[++i];
        else if (!strcmp(argv[i], "--opponent") && i + 1 < argc) flow_start_opp = argv[++i];
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
    host_set_sim_ticks(sim_ticks > 0 ? sim_ticks : classic ? HOST_ORIGINAL_SIM_TICKS : HOST_DEFAULT_SIM_TICKS);
    gfx_init();      /* EGA model, frame source */
    timer_init();    /* host tick handler, timer routines */
    input_init();    /* INT 9 handler, getkey code pointers */
    enh_init(!classic, draw_distance);   /* ENH: overlay (off in the classic mode) */
    int rc = game_main();
    host_shutdown();
    return rc;
}
