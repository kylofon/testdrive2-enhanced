# Enhanced renderer — design

The faithful engine (see `ENGINE.md`) runs unchanged: simulation, game flow, cockpit, mirror and HUD.
`src/enhanced/` renders the front view (screen rows 19–110) again on the host side and lays it over the
EGA frame. Goals of this stage: **smooth 60 fps motion** and **a longer draw distance**, using only the
original data (no new assets: the 16 EGA colours and the original sprites, fonts and stage data).

## Frame

`run_stage` (`game/scene.c`) keeps running the original frame every iteration (projection, buffer
drawing, mirror, HUD, present, cockpit), so every visible element the original draws still exists and
all state stays faithful. Hooks, marked `ENH:` in the engine:

| Hook | Where | Purpose |
|---|---|---|
| `enh_init()` | `main.c` | output scale, overlay installation |
| `enh_stage_begin()` / `enh_stage_end()` | `run_stage` after `stage_load` / before returning | sprite decoding, buffers; overlay on / off |
| `enh_life_reset()` | after `life_reset` / `traffic_resync` | snap interpolation state |
| `enh_sim_step()` | `sim_timer_routine`, after each 10 Hz simulation step | interpolation snapshots |
| `enh_before_overlays()` | `run_stage` after `draw_front` | snapshot of the main buffer (coverage) |
| `enh_frame()` | `run_stage` after `present_main_view` | render the road window |
| `enh_gear_gate()` | replaces `draw_gear_gate` in the loop | keeps the frame-counted close delay at the original speed |

Frame pacing: `--frame-rate` defaults to 60. The original PC drew about 15 frames per second
(`HOST_ORIGINAL_FPS`); the only frame-counted behaviour in TD2 is the gear-gate close delay (10 frames),
which `enh_gear_gate` advances at 15 Hz.

## Smooth motion

The simulation moves everything 10 times per second (every 10th timer tick): the player's road position
(`player_pos` = DS address of the road byte, `player_sub` low byte = 0..255 sub-units), lateral
position, heading / yaw, the opponent, the police and the traffic cars, and the mountain / cloud scroll.
The original projects whole road units only (row i always at depth i+4).

* `enh_sim_step` records the state after each step together with the host time, and the previous state.
* A frame uses the state extrapolated from the last step by `alpha = time since that step / 0.1 s`,
  clamped to [0, 1]: positions advance by the last step's delta (unit crossings and the road length wrap
  included), laterals and angles likewise. Large jumps (restart, crash reset, stage start) snap.
* The car's continuous position is `s = unit + sub/256`. Road unit `u` (the byte at unit `u`) is at
  depth `z = (u − s) + 3`; with `sub = 0` this is exactly the original's `i + 4` for `u = unit + 1 + i`.
* The road integrators (pitch → height, curve → heading → lateral, with the original clamps and `tan256`
  table) are evaluated per unit exactly as in `project_front_rows`, starting one unit behind the car.
  Their origin is interpolated between the car's unit and the next one by the sub-unit fraction, so
  crossing a unit does not shift the view.
* Unit-based phases (centre-line dashes, poles and tunnel lights every 16 units, scenery ring slot) come
  from the unit index `u`, so they move with the road.

## Projection

In the original's buffer units (320 × 92), with `X` the integrated lateral sum and `H` the height sum:
`x = 125 + X · 2.9794 / z`, `y = 51 + H · 2.1465 / z`, road half-width `W = 1200 / z` (the tables at
§4.4 of `scene_render.md`). The renderer evaluates the same formulas in floating point at continuous
`z`, scaled to the output resolution.

## Draw distance

* **Road:** `ENH_ROWS` (180) units instead of 60. Crest occlusion works as in the original: rows are
  processed near → far, each scanline is drawn once, and objects of a row are clipped to the top of the
  nearer rows.
* **Traffic, opponent, police:** the traffic lists hold the whole stage, so cars are drawn up to the
  road's end of view.
* **Road objects** (signs, bands, gas station / FINISH, hazards) come from the road records: drawn at
  any distance.
* **Scenery sprites and text signs** come from the simulation's 128-slot ring, filled 70 units ahead
  (`motion`, slot = counter + 0x46). The enhanced engine fills it `ENH_SCENERY_AHEAD` (120) units ahead
  instead; the look-ahead region state and the right-side zone test move with it (`ENH:` in
  `sim_motion.c` and the stage start), so the same random numbers produce the same objects, just
  earlier. The first 120 units of a stage still start empty, as the first 70 did.
* Far objects are drawn with the smallest sprite, scaled down, and fade in over the last 10 % of the
  distance (the only blending besides edge smoothing).

## Pixels

* The renderer draws **palette indices** into a sample buffer at `S × Q` times the original resolution
  (`S` = output scale, `--res-scale`, default 4; `Q` = supersampling, 2, or 4 at scale 1), with the
  original operations: fills and spans in DAT colours, sprites through their AND mask and OR / XOR image
  exactly as the blitters combine planes. Sprites are sampled nearest-neighbour at their continuous size,
  choosing the original's size variant for the projected width (`scale4` / `scale5` / `carscale`),
  scaled relative to that variant's nominal width. Hot spots and offsets as in the blitters.
* Resolve: each output pixel averages its samples through the current palette (the crash flash swaps
  the palette). Rendering is split into bands on the host worker pool.
* **Kept from the EGA image:** everything the original draws over the road view after the road itself:
  the mirror and its frame, the ticket, and anything drawn on the screen after the buffer is presented
  (messages, windscreen cracks, GAME OVER). Coverage = pixels of the main buffer that changed between
  `enh_before_overlays` and the present, plus VRAM pixels in the window that differ from what was
  presented.
* **Not replaced:** the falling-off-the-road view (`fall_mode` ≠ 0) shows the original image.

## Command line

| Option | Default | Meaning |
|---|---|---|
| `--frame-rate FPS` | 60 | drawing rate while driving (`0` = unpaced) |
| `--res-scale N` | 4 | output = 320×200 × N (1–8) |
| `--draw-distance N` | 180 | road units drawn (60–240; scenery is limited to 120) |
| `--classic` | off | original renderer and 15 fps (for comparison) |
