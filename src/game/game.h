#pragma once
/* Cross-subsystem prototypes of the game code. Each module implements its own functions (see
 * PORTING.md for ownership); internal helpers stay static or in the module's own header
 * (game/flow.h, game/scene.h, game/sim.h). Names follow port/symbols.csv. All game state is in mem[]
 * (symbols.h offsets). */
#include "../mem.h"
#include "../symbols.h"

/* ---- game_flow (game/flow*.c) ------------------------------------------------------------------ */
int  game_main(void);                   /* 0000:07b3 main */

/* ---- scene_render (game/scene*.c) -------------------------------------------------------------- */
s16  run_stage(void);                   /* 06c9:1b2c stage runner: returns DS:5490 sign-extended (-1 = quit) */

/* ---- simulation (game/sim*.c) ------------------------------------------------------------------
 * The simulation module also owns 06c9:3f40 and 06c9:3ff9, which sit in scene_render's address range
 * but are documented in simulation.md. */
void sim_stage_start(void);             /* 06c9:3f40 stage reset; registers sfx_tick and drive_tick in the timer list */
void sim_restart_reset(void);           /* 06c9:3ff9 per-life reset */
void traffic_resync(void);              /* 06c9:5ada */
void police_reset(void);                /* 06c9:5427 */
void sim_timer_routine(void);           /* 06c9:403b timer routine (registered with codeptr) */
void steer_update(void);                /* 06b3:0008 (C code in segment 06b3, called from 06c9:420a) */
