#pragma once
/* Enhanced renderer (ENHANCED.md). Not bound by the faithful-engine rules in ENGINE.md.
 *
 * The faithful stage loop keeps running; this module renders the front view (screen rows 19..110) and the
 * rear-view mirror again at a higher resolution, with smooth motion and a longer view, and lays them over
 * the EGA frame. The hooks below are called from the engine at the places marked `ENH:`. */
#include "../types.h"

#define ENH_DEFAULT_ROWS  180      /* road units drawn (the original draws 60) */
#define ENH_MIN_ROWS      60
#define ENH_MAX_ROWS      240
#define ENH_SCENERY_AHEAD_MAX 120  /* scenery ring filled this many units ahead (the original: 70) */
extern int enh_scenery_ahead;      /* ENH_SCENERY_AHEAD_MAX, or the original's 70 with --classic */
#define ENH_SCENERY_AHEAD enh_scenery_ahead

void enh_init(bool enabled, int rows);  /* main.c: overlay installation (enabled = false: --classic) */
extern bool enh_show_position;    /* --show-position: the stage code and road unit on the screen (F9) */
extern bool enh_valley;           /* --valley: true = on (the valley floor below drop-offs), false = mist */
extern bool enh_detail_max;       /* --sprite-detail: true = max (the largest sprite variant everywhere) */
extern bool enh_road_bands;       /* --enhanced-road: the road and shoulders alternate between two shades */
extern bool enh_side_bands;       /* --enhanced-sides: the ground beside the road alternates as well */

void enh_stage_begin(void);       /* run_stage, after stage_load: sprite cache, state */
void enh_stage_end(void);         /* run_stage, before returning: overlay off */
void enh_life_reset(void);        /* run_stage, after life_reset / traffic_resync: snap interpolation */
void enh_sim_step(void);          /* sim_timer_routine, after each 10 Hz simulation step */
void enh_unit_step(void);         /* motion, after each road unit (yaw and lateral change per unit) */
void enh_before_overlays(void);   /* after draw_front: main buffer snapshot (coverage) */
void enh_after_mirror(void);      /* after draw_mirror: main buffer snapshot (coverage of the mirror) */
/* draw_mirror_objects (scenery): ring slot k as the mirror sees it. With the ring filled further ahead than
 * the original's, the slots of units behind the car are reused; this gives the values the slot had when the
 * car passed the unit (false: keep the ring's). */
bool enh_mirror_scenery(u8 k, s8 *type, s8 *offset);
void enh_frame(void);             /* after present_main_view: render the road window */
void enh_gear_gate(void);         /* replaces draw_gear_gate: close delay counted at the original 15 Hz */

/* Developer aid (run_game_load_stage, attract mode): TD2_ENH_STAGE=<scenery code><stage>, e.g. CCC3,
 * makes the attract mode drive that stage. */
void enh_debug_stage(void);
/* Selects the scenery and stage of a code as TD2_ENH_STAGE takes it (e.g. "CCC3", "EC_0"); false if
 * SCENES.DAT has no such stage. */
bool enh_select_stage(const char *code_stage);

/* Map viewer (--viewer, enh_viewer.c, ENHANCED.md "Map viewer"): a camera flown along a stage, the enhanced
 * road view filling the window, without the simulation, the cockpit or other cars. main.c sets the stage;
 * game_main hands over to flow_stage.c run_viewer, which loads the stage's files and calls enh_viewer_run. */
extern const char *enh_viewer_stage;  /* --viewer <scenery code><stage> (NULL: the game) */
extern int enh_viewer_start;          /* --viewer-start: the road unit to start at */
void enh_viewer_run(void);            /* runs the viewer until Esc (the stage's files are loaded) */

/* Developer aid (sim.c decode_controls, sim_motion.c motion, sim_ai.c demo_steer): TD2_ENH_DRIVER=follow
 * or weave makes the attract mode steer like a player (steering input, yaw integration) instead of the
 * demo's fixed yaw; weave also changes lanes every 3 s, lazy steers only now and then, offleft / offright /
 * offwater drive off the road (see ENHANCED.md). */
bool enh_dev_driver(void);
void enh_dev_steer(void);
