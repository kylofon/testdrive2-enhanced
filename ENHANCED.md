# Enhanced renderer — design

The faithful engine (see `ENGINE.md`) runs unchanged: simulation, game flow, cockpit, mirror and HUD.
`src/enhanced/` renders the front view (screen rows 19–110) again on the host side and lays it over the
EGA frame. Goals of this stage: **smooth 60 fps motion** and **a longer draw distance**, using only the
original data (no new assets: the 16 EGA colours and the original sprites, fonts and stage data).

Files: `enhanced.c` (hooks, snapshots and extrapolation, coverage, overlay, developer aids),
`enh_scene.c` (the front view in continuous depth, turned into a display list), `enh_raster.c` (sprite
decoding, sample buffer, rasterisation, resolve), `enh_internal.h`.

## Frame

`run_stage` (`game/scene.c`) keeps running the original frame every iteration (projection, buffer
drawing, mirror, HUD, present, cockpit), so every visible element the original draws still exists and
all state stays faithful. Hooks, marked `ENH:` in the engine:

| Hook | Where | Purpose |
|---|---|---|
| `enh_init()` | `main.c` | overlay installation (not in `--classic`) |
| `enh_stage_begin()` / `enh_stage_end()` | `run_stage` after `stage_load` / before returning | sprite cache, state; overlay off |
| `enh_life_reset()` | after `life_reset` / `traffic_resync` | snap interpolation state |
| `enh_sim_step()` | `sim_timer_routine`, after each 10 Hz simulation step (`sim_step`) | interpolation snapshots |
| `enh_unit_step()` | `motion`, after each road unit | per-unit samples of the view yaw and lateral |
| `enh_before_overlays()` | `run_stage` and `crash_sequence` after `draw_front` | snapshot of the main buffer (coverage) |
| `enh_frame()` | `run_stage` and every `crash_sequence` step, after `present_main_view` | render the road window |
| `enh_gear_gate()` | replaces `draw_gear_gate` in the loop | keeps the frame-counted close delay at the original speed |
| `enh_debug_stage()` | `run_game_load_stage`, attract mode | developer aid (`TD2_ENH_STAGE`) |
| `enh_dev_driver()` / `enh_dev_steer()` | `decode_controls`, `motion`, `demo_steer` | developer aid (`TD2_ENH_DRIVER`) |

The crash sequence redraws the front view and presents it seven times with the windscreen cracks drawn
into the main buffer, so it gets the same two hooks as the loop: the road stays enhanced (with the crash
flash palette) and the hood damage and cracks are kept from the EGA image.

Frame pacing: `--frame-rate` defaults to 60. The original PC drew about 15 frames per second
(`HOST_ORIGINAL_FPS`); the only frame-counted behaviour in TD2 is the gear-gate close delay (10 frames),
which `enh_gear_gate` advances at 15 Hz (other frames do not count down).

## Smooth motion

The simulation moves everything 10 times per second (every 10th timer tick): the player's road position
(`player_pos` = DS address of the road byte, `player_sub` low byte = 0..255 sub-units), lateral
position, heading / yaw, the opponent, the police and the traffic cars, and the mountain / cloud scroll.
The original projects whole road units only (row i always at depth i+4).

* `enh_sim_step` records the state after each step together with the step's tick time
  (`host_tick_ns`), and the previous state.
* A frame uses the state extrapolated from the last step by `alpha = time since that step / 0.1 s`,
  clamped to [0, 1]. Road positions advance
  by the next step's predicted advance: a driver moves `speed_hi * 3` sub-units per step with that step's
  speed, and the 16-bit speed is extrapolated too, so accelerating cars do not jump at each step (when
  the last advance was not a normal speed step, the last delta is used). Unit crossings and the road
  length wrap are included; large jumps (restart, crash reset, stage start) snap; while the drive result
  is set (crash, messages) or the car is falling nothing is extrapolated.
* **View yaw and lateral.** `motion` changes `yaw`, `view_yaw` and `player_x` once per road unit crossed
  (`yaw += curve + steering`, `player_x -= sin(yaw)·36/256`), and a step crosses a varying number of
  units (1 or 2 at 100 mph), so extrapolating them per step made everything on screen jerk sideways
  when steering and in bends. `enh_unit_step` records them after every unit (the lateral as the running
  sum of the per-unit yaw term); the rest of the lateral (pull-over, the demo's lane changes, resets)
  changes per step and is interpolated between the last two steps. The per-unit samples are read as a
  piecewise-linear function of the road position, at the position the car was drawn at one step ago,
  minus one unit: those units are always already recorded, so nothing is predicted and nothing jumps
  when the next step arrives (units past the last step are predicted with `motion`'s formulas only as a
  fallback after stalls). The cost is latency: steering turns and slides the view 100 ms plus one road
  unit (57 ms at 150 mph, 85 ms at 100 mph, 140 ms at 60 mph) after the simulation.
* **Only the steering part of yaw is delayed.** Each unit adds the road curve of the unit left
  (`road_curve`) to `yaw`, together with the steering / skid term, while the road rows shift by the same
  unit, so in the original the two cancel and the view follows the road. Delaying the whole of `yaw`
  against the undelayed road turned the view into every bend and back out of it for a few units, without
  any steering. `yaw` is therefore split into the road curve summed over the units entered (known from
  the road bytes for every unit) and the rest (steering, skidding, clamps): the rest is read at the
  delayed position, the curve sum at the car's unit and the next one for the two blended views, as the
  original pairs them; then the ±0x2800 clamp and `view_yaw = yaw / 4`. The demo keeps its own
  `view_yaw` (delayed samples); the pull-over zeroing comes through the samples. The other cars' laterals, which move per step and stop at limits, are interpolated
  between the last two steps (100 ms).
* The car's continuous position is `s = unit + sub/256`. Road unit `u` (the byte at unit `u`) is at
  depth `z = (u − s) + 3`; with `sub = 0` this is exactly the original's `i + 4` for `u = unit + 1 + i`.
* The road integrators (pitch → height, curve → heading → lateral, with the original clamps and `tan256`
  table) are evaluated per unit as in `project_front_rows`, starting one unit behind the car (that unit
  lies on the car's own slope and heading). Two views are integrated, with the car at its unit and at
  the next one, and blended by the sub-unit fraction, so crossing a unit does not shift the view.
* **Deviation:** the original indexes `tan256` by whole degrees. Bends and hills change the heading and
  pitch accumulators by fractions of a degree per unit (1/16° for the gentlest bend), so with whole
  degrees the road turned in 1° steps, each concentrated in one unit of travel: a sideways jerk of the
  whole picture every few units in every bend. The table is interpolated within the degree instead
  (identical at whole degrees); the road shape differs from the original's by less than a degree of
  heading.
* Mountain and cloud scroll add each unit's curve (`heading += curve/2`, clouds `+ 5/4` of that) as the
  car moves through the unit; the 1 Hz cloud drift is not extrapolated.
* Unit-based phases (centre-line dashes, poles and tunnel lights every 16 units, scenery ring slot) come
  from the unit index `u`, so they move with the road. Cars are placed at their continuous road position
  (the original draws them at their whole unit).

## Projection

In the original's buffer units (320 × 92), with `X` the integrated lateral sum and `H` the height sum:
`x = 125 + X · 2.9794 / z`, `y = 51 + H · 2.1465 / z`, road half-width `W = 1200 / z` (the tables at
§4.4 of `scene_render.md`). The renderer evaluates the same formulas in floating point at continuous
`z`, and runs the rest of the original's front view code on these rows in floating point: edges with the
lane widening, the far-right band, the cut lines of cliffs and drop-offs, the tunnel rows, the scanline
spans, the sky / ground / tunnel wall drawing and the per-row objects in the original's order. The result
is a display list in original coordinates, rasterised at the output resolution.

## Draw distance

* **Road:** `ENH_ROWS` (`--draw-distance`, 180) units instead of 60. Crest occlusion works as in the
  original: rows are processed near → far, each scanline belongs to the nearest pair of rows that covers
  it, and objects of a row are clipped to the top of the nearer rows. The scanlines below the nearest
  row are interpolated toward the unit behind the car (the original extrapolates its first two rows).
* **The original's per-frame state stays within its 60 rows** (`CUT_ROWS`): the cliff and drop-off cut
  lines (whose walls reach the top of the view) and the entrance of the tunnel handled by the original's
  variables. Only the far end of that tunnel is looked for at any distance. Beyond 60 rows:
  * cliff rows are drawn as wall segments of their own height (`W` pixels above the road, about 560
    height units) along the outer edge between neighbouring rows, reaching `W` outwards, so a far cliff
    is a ridge that follows the road and becomes the original's wall when it comes within 60 units;
  * a second tunnel (the original never has two in view) keeps its own entrance / far-end values and is
    drawn with the original's mouth and wall code, clipped below the nearer tunnel's ceiling;
  * a non-style tunnel that starts beyond 60 units shows its ribs and lights until the original's portal
    takes over at 60 units (its portal fills everything above it, which only fits a near tunnel);
  * tunnel ends hidden behind a crest are taken at the crest for the wall scanlines;
  * objects beyond the nearest tunnel's far end are clipped to its opening.
* **Traffic, opponent, police:** the traffic lists hold the whole stage, so cars are drawn up to the
  road's end of view (from one unit ahead, like the original).
* **Road objects** (signs, bands, gas station / FINISH, hazards) come from the road records: drawn at
  any distance.
* **Scenery sprites and text signs** come from the simulation's 128-slot ring, filled 70 units ahead
  (`motion`, slot = counter + 0x46). The enhanced engine fills it `ENH_SCENERY_AHEAD` (120) units ahead
  instead (`ENH:` in `sim_motion.c` and `sim_stage_start`); everything that decides a slot's content
  moves with it, so each slot gets the value the original would give it:
  * the look-ahead region state (`lookahead_flags`, initialised from the first 121 road bytes) and the
    right-side zone test;
  * the scenery density: the density objects (`49db` / `49e1`) of the next 50 units are applied first;
  * placed objects (`499a`, codes 0x1C–0x2F, which include the SGN text signs) are written when their
    slot is filled, from the road byte 51 units further on (the original's own write, 69 units ahead,
    then stores the same value).
  At stage start the slots 71..120, which the original had always filled before they came into view,
  are cleared and given their placed objects; slots 0..70 keep the DAT's contents as before. The random
  numbers are drawn at the same moments as before, but a slot is decided 50 units earlier, so the random
  scenery differs from the original's. `--classic` uses the same ring, so the two modes show the same
  objects.
* Far objects fade in over the last 10 % of their distance (the draw distance, or 120 units for scenery),
  dithered in the sample buffer (the only blending besides edge smoothing).

## Pixels

* The renderer draws **palette indices** into a sample buffer at `S × Q` times the original resolution
  (`S` = output scale, `--res-scale`, default 4; `Q` = supersampling, 2, or 4 at scale 1), with the
  original operations: fills and spans in DAT colours, lines, sprites through their AND mask and OR / XOR
  image exactly as the RAM blitters combine planes (clear / set nibbles, stored planes, replace order).
* **Sprite sizes:** the original's size variant is chosen for the projected width (`scale4` / `scale5` /
  `carscale`). The original draws each variant unscaled on the rows that select it, and the variants are
  not proportional to distance. Here a group has one continuous height, linear between each variant's
  height at the centre of its original depth range and proportional to `W` outside the original's 60
  rows; the chosen variant is scaled to that height (nearest-neighbour), so sizes match the original and
  do not jump when the variant changes. Hot-spot pixels stay centred on the anchor. Scenery groups whose
  variants all have about the same height (the CCC redwood trunks, cut off by the top of the view
  wherever the original draws them) are drawn only within the original's scenery distance (44 rows),
  fading in over the 5 units beyond it; scaled down they would be free-standing columns. The cliff, portal,
  mountain and cloud sprites are drawn at their original size.
* **Road markings:** the original sets one pixel per road unit (centre dot, lane lines). Here they are
  strips along the road surface whose on / off state comes from the unit under each scanline, 1 pixel
  wide up to the original's distance and thinner beyond; other 1-pixel lines (tunnel ribs, FINISH
  letters, sign text) are thinned the same way.
* Resolve: each output pixel averages its samples in linear light through the current palette (the crash
  flash swaps the palette; a palette change re-resolves). Rendering and resolving are split into bands on
  the host worker pool.
* **Kept from the EGA image:** everything the original draws over the road view after the road itself:
  the mirror and its frame, the ticket, and anything drawn on the screen after the buffer is presented
  (messages, windscreen cracks, GAME OVER). Coverage = pixels of the main buffer that changed between
  `enh_before_overlays` and the present, the mirror rectangle, the opaque pixels of the mirror frame and
  the ticket area, plus VRAM pixels in the window that differ from what was presented.
* **Not replaced:** the falling-off-the-road view (`fall_mode` ≠ 0) shows the original image.

## Command line

| Option | Default | Meaning |
|---|---|---|
| `--frame-rate FPS` | 60 | drawing rate while driving (`0` = unpaced) |
| `--res-scale N` | 4 | output = 320×200 × N (1–8) |
| `--draw-distance N` | 180 | road units drawn (60–240; scenery is limited to 120) |
| `--classic` | off | original renderer and 15 fps (for comparison) |

## Developer aids (environment variables)

| Variable | Effect |
|---|---|
| `TD2_SNAPSHOT_DIR`, `TD2_SNAPSHOT_INTERVAL_MS`, `TD2_SNAPSHOT_START_S`, `TD2_SNAPSHOT_COUNT` | save presented frames (interval 0 = every frame) |
| `TD2_ENH_STAGE=<code><stage>` | the attract mode drives that stage (e.g. `CCC3`, `EC_0`) |
| `TD2_ENH_START=<unit>` | the attract mode starts that many units into the stage |
| `TD2_ENH_COMPARE_DIR=<dir>` | no extrapolation, whole units; every 2 s `cmpNNNN.bmp` (enhanced window above the original's) and `cmpNNNN.txt` (rows, state, display list) |
| `TD2_ENH_STATS=1` | render / overlay times every 300 frames on stderr |
| `TD2_ENH_TRACE=<file>` | per-frame values: time, position, lateral, view yaw, scroll, screen x of the road centre 10 / 30 / 60 units ahead, a tracked car's id / screen x / distance, step position, render ms, camera heading drawn (road curve sum / 4 − view yaw) and the simulation's at its last step, steering angle, road curve, delayed read position, steering part of yaw |
| `TD2_ENH_DRIVER=follow` / `weave` / `lazy` | the attract mode steers like a player (steering input and yaw integration instead of the demo's fixed yaw); `weave` changes lanes every 3 s, `lazy` only steers in 2 of 10 steps (steering held through bends) |
| `TD2_ENH_DEBUG=1` | sprite group sizes at stage start |
