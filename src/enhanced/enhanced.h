#pragma once
/* Enhanced renderer (ENHANCED.md). Not bound by the faithful-engine rules in ENGINE.md.
 *
 * The faithful stage loop keeps running; this module renders the front view (screen rows 19..110) again
 * at a higher resolution, with smooth motion and a longer draw distance, and lays it over the EGA frame.
 * The hooks below are called from the engine at the places marked `ENH:`. */
#include "../types.h"

#define ENH_DEFAULT_ROWS  180      /* road units drawn (the original draws 60) */
#define ENH_MIN_ROWS      60
#define ENH_MAX_ROWS      240
#define ENH_SCENERY_AHEAD 120      /* scenery ring filled this many units ahead (the original: 70) */

void enh_init(bool enabled, int rows);  /* main.c: overlay installation (enabled = false: --classic) */

void enh_stage_begin(void);       /* run_stage, after stage_load: sprite cache, state */
void enh_stage_end(void);         /* run_stage, before returning: overlay off */
void enh_life_reset(void);        /* run_stage, after life_reset / traffic_resync: snap interpolation */
void enh_sim_step(void);          /* sim_timer_routine, after each 10 Hz simulation step */
void enh_before_overlays(void);   /* after draw_front: main buffer snapshot (coverage) */
void enh_frame(void);             /* after present_main_view: render the road window */
void enh_gear_gate(void);         /* replaces draw_gear_gate: close delay counted at the original 15 Hz */

/* Developer aid (run_game_load_stage, attract mode): TD2_ENH_STAGE=<scenery code><stage>, e.g. CCC3,
 * makes the attract mode drive that stage. */
void enh_debug_stage(void);
