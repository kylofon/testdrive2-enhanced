# Enhanced renderer — design

The faithful engine (see `ENGINE.md`) runs unchanged: simulation, game flow, cockpit and HUD.
`src/enhanced/` renders the front view (screen rows 19–110) and the rear-view mirror again on the host
side and lays them over the EGA frame. Goals of the first stage: **smooth 60 fps motion** and **a longer draw distance**, using only the
original data (the 16 EGA colours and the original sprites, fonts and stage data). The second stage adds **new assets**
drawn by the renderer (see "New assets"): colours beyond the 16 EGA ones, mixed from the stage's own colours; the
original sprites are kept.

Files: `enhanced.c` (hooks, snapshots and extrapolation, coverage, overlay, developer aids),
`enh_scene.c` (the front view and the mirror in continuous depth, turned into display lists),
`enh_raster.c` (sprite decoding, sample buffers, rasterisation, resolve), `enh_internal.h`.

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
| `enh_after_mirror()` | `run_stage` and `crash_sequence` after `draw_mirror` | second snapshot: coverage inside the mirror |
| `enh_mirror_scenery()` | `draw_mirror_objects` (scenery) | the ring slots of units behind the car (see Mirror) |
| `enh_frame()` | `run_stage` and every `crash_sequence` step, after `present_main_view` | render the road window and the mirror |
| `enh_gear_gate()` | replaces `draw_gear_gate` in the loop | keeps the frame-counted close delay at the original speed |
| `enh_debug_stage()` | `run_game_load_stage`, attract mode | developer aid (`TD2_ENH_STAGE`) |
| `enh_dev_driver()` / `enh_dev_steer()` | `decode_controls`, `motion`, `demo_steer` | developer aid (`TD2_ENH_DRIVER`) |
| written mask | `platform/gfx_ega.h` (`ega_write`), `gfx.c`, `platform/prompts.c` (boxes, joystick calibration) | coverage of everything drawn on the screen (see Pixels) |
| scripted keys, held frames | `host.c` | developer aids (`TD2_KEYS`, `TD2_SNAPSHOT_HELD`) |

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
  is set (crash, messages) nothing is extrapolated. While the car falls the original keeps simulating
  and drawing the current view, so the view keeps moving here too, and `fall_scroll` is extrapolated
  (it grows by an increasing amount per step, 3 per step in the water).
* **View yaw and lateral.** `motion` changes `yaw`, `view_yaw` and `player_x` once per road unit crossed
  (`yaw += curve + steering`, `player_x -= sin(yaw)·36/256`), and a step crosses a varying number of
  units (1 or 2 at 100 mph), so extrapolating them per step made everything on screen jerk sideways
  when steering and in bends. `enh_unit_step` records them after every unit (the lateral as the running
  sum of the per-unit yaw term); the rest of the lateral (pull-over, the demo's lane changes, resets)
  changes per step and is interpolated between the last two steps. The per-unit samples are read as a
  piecewise-linear function of the road position, half a unit behind the car's drawn position, and what
  comes out is smoothed with a first-order filter of 110 ms (units past the last step are predicted with
  `motion`'s formulas only as a fallback after stalls). The read position has to stay behind the last
  recorded sample, and what is left of the 10 Hz steps is taken out by the filter instead of by a longer
  delay: measured against the plain delay of 100 ms plus one unit that this replaces, the frame-to-frame
  jerk of the view yaw is 25–46 % lower (5 % higher with the `follow` driver) and of the lateral 12–79 %
  lower, while the delay drops from 242 / 185 / 157 ms to 181 / 153 / 138 ms at 60 / 100 / 150 mph.
  The filter is reset at a stage or life start and skipped in compare mode.
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

## Mirror

The mirror (80 × 17 pixels at 240, 8 of the road window) is the same renderer with the view's parameters
(`view_setup` in `enh_scene.c`, the original's `scene_mirror_view`): it walks the road backwards from the
car's unit, its heading starts at `-view_yaw`, its laterals are halved, and its tables are
`x = 40 + X · 1.6237 / z`, `y = 8 + H · 0.8747 / z`, `W = 360 / z` with `z = row + 6`. Row j of the
enhanced mirror is the unit `car unit + 1 - j` at depth `j + 5 + frac`, so the rows, the state machine,
the ground and the objects are the front view's code.

* **Longer view:** 75 units instead of 25 (the front distance × 25/60).
* **The original's differences are kept:** road signs are drawn as masks without their image, no FINISH
  letters, no cliff decorations, the cross bands run from three rows farther to the row, traffic shows
  its front / rear sprites swapped, the opponent and the police their front sprites, the police light
  bar flashes, the parked police car is drawn one row nearer, tunnel portals use the mirror's sprites
  and offsets, the mountains (`rmt0-2`) scroll at half the front speed on the original's farthest row
  and there are no clouds.
* **Cars** are shared with the front view: a car `d` units ahead of the player belongs to the front view
  for `d >= 1` and to the mirror below that, as in the original's two snapshots.
* **Scenery behind the car.** The simulation's 128-slot ring is filled 120 units ahead here, which reuses
  the slots of the units more than 8 units behind the car, so the mirror cannot read them. `enh_unit_step`
  records each unit's slot as the car passes it, and both the enhanced mirror and the faithful one
  (through `enh_mirror_scenery`) read that history instead. With `--classic` the ring is the original's
  70 units and nothing of this applies.
* **Falling off the road:** the mirror shows the original's fills (sky above, cliff below a line that
  moves down with `fall_scroll / 8`; in the water its last image scrolls up and colour 9 fills below).
* The mirror frame (`mirr`), the HUD and anything else drawn over the mirror stay EGA, through a second
  main-buffer snapshot taken after `draw_mirror` (`enh_after_mirror`).

## Draw distance

* **Road:** `ENH_ROWS` (`--draw-distance`, 180) units instead of 60. Crest occlusion works as in the
  original: rows are processed near → far, each scanline belongs to the nearest pair of rows that covers
  it, and objects of a row are clipped to the top of the nearer rows. The scanlines below the nearest
  row are interpolated toward the unit behind the car (the original extrapolates its first two rows).
* **The original's per-frame state stays within its 60 rows** (`CUT_ROWS`): the cliff and drop-off cut
  lines (whose walls reach the top of the view) and the entrance of the tunnel handled by the original's
  variables. Only the far end of that tunnel is looked for at any distance. Beyond 60 rows:
  * **rock faces** (drawn for every cliff row, see "New assets") are objects of the world with a height of
    their own above the road, so they continue beyond 60 units without a step and only grow by perspective
    as the car approaches; far away they fade out over the last tenth of the drawn distance;
  * a tunnel of either style that starts beyond 60 units (or beyond another tunnel) keeps its own
    entrance / far-end values and is drawn with the original's mouth and wall code; its entrance is the
    hill it goes into, as high as the rock face of that row and sloping down to both sides with the mouth
    cut out, which grows into the original's portal as the tunnel comes within 60 units;
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
  fading in over the 5 units beyond it; scaled down they would be free-standing columns. The portal,
  mountain and cloud sprites are drawn at their original size.
* **Car sizes** (traffic, opponent, police, the parked police car; front view and mirror), as in Test Drive
  Enhanced: instead of the original's variant for the row (`carscale`, and beyond its rows the smallest),
  the most detailed variant is used that is still drawn at `CAR_LOD_MIN` = half its own size or larger (at
  the group's continuous height), so cars are mostly scaled down from a larger, more detailed sprite, and the
  smallest variant, whose heavy outline stands out, is never used (the next one is scaled down instead).
* **Mountains and clouds** stay where the original puts them: on the horizon of the original's 60 rows
  (`top_sy_near`) and on its farthest row, not on the highest point of all 180 rows — a climb 60 to 180
  units ahead would otherwise lift them into the sky (California stage 1). The ground of the far rows is
  drawn after them and covers them where the road climbs above that horizon, as a hill in front of them
  would.
* **Road markings:** see "New assets". Other 1-pixel lines (tunnel ribs, FINISH letters, sign text) are
  1 pixel wide up to the original's distance and thinner beyond.
* Resolve: each output pixel averages its samples in linear light through the current palette (the crash
  flash swaps the palette; a palette change re-resolves). Rendering and resolving are split into bands on
  the host worker pool.
* **Kept from the EGA image:** everything the original draws over the views after the road itself:
  the mirror frame, the ticket, and anything drawn on the screen after the buffer is presented
  (drive result messages, lives left, windscreen cracks, engine smoke, GAME OVER, the pause and exit
  prompts, the joystick calibration screen). Coverage = pixels of the main buffer that changed between
  `enh_before_overlays` and the present (inside the mirror: between `enh_after_mirror` and the present),
  the opaque pixels of the mirror frame and the ticket area, VRAM pixels in the window that differ from
  what was presented, and every VRAM pixel written since the present.
* **Written mask.** The EGA model keeps one bit per VRAM pixel that is set by every write through the
  adapter (any write mode, bits selected by the bit mask), whatever value it leaves; `enh_frame` clears it
  after presenting. Comparing values alone missed pixels drawn in the colour they already had: a message
  box is filled black, and wherever the presented EGA image was already black (road edges, tyres, sign
  posts, shadows) the enhanced road stayed visible inside the box. The prompts that save the screen under
  them and restore it (pause, exit, status strip, joystick calibration) save the mask with it and put it
  back with the pixels, so the restored area shows the enhanced road again at once instead of the saved
  EGA image for a frame.
* **Falling off the road** (`draw_front`, `fall_mode` ≠ 0): the original scrolls its finished view up by
  `fall_scroll` (and stops drawing the view once that reaches 92) and fills the area below it: for a
  drop to the left the sky colour left of `left_sky_x` and brown right of it, for a drop to the right
  brown left of `right_sky_x` and the sky colour right of it, both 180 rows high with brown below; in the
  water colour 9. The enhanced view is drawn the same way: the display list is shifted up by the
  (extrapolated) `fall_scroll` in output coordinates, so it scrolls smoothly, and the fills use the
  original's `left_sky_x` / `right_sky_x` and colours in window coordinates. The mirror keeps its
  original falling image. There is no switch between renderers at the start or the end of the fall; the
  crash sequence that follows is the usual one.

## New assets

**Colours.** The sample buffers hold palette indices: 0..15 are the EGA palette registers as displayed (so
the crash flash still applies), 16..255 are extended colours (`EXT_*` in `enh_internal.h`), each a mix of
palette registers in linear light — `((1 − t)·a + t·b)·k`, then mixed towards `c` by `h` — so they follow
the palette too and are built from the stage's own colours (`enh_colours_setup`). Every extended colour also
has an EGA colour of its own (`enh_base`): sprites combine with that one through their AND / OR / XOR
tables, and a pixel a sprite leaves unchanged keeps its extended colour. The resolve averages the samples
through all 256 colours; the palette key includes the extended colours.

* **Road markings.** The original sets one pixel per road unit: the centre line (plane 0 cleared, planes
  1–3 set, which always gives colour 14) where the road is wide or the dash phase (bit 2 of the unit
  counter) is on, and the lane lines (colour 15) where both are. Here they are strips `MARK_W` = 0.05 of
  the road's half-width wide (about 1 pixel at the original's farthest row, 14 at the bottom of the view),
  at least one output pixel; a strip narrower than that is drawn one output pixel wide in a mix of the road
  colour and the marking colour by its coverage (`EXT_MARK_C` / `EXT_MARK_L`, 8 levels; on other colours
  than the road's, such as the tunnel floor, the marking colour dithered by the coverage). The dash phase is
  box-filtered over the depth each sample row covers, so far away, where a dash is less than a scanline
  deep, the dashes turn into a steady faint line instead of flickering from frame to frame.
* **Road pattern.** The road (colour 7) and the shoulders (the stage's shoulder colour) alternate between
  their colour and a slightly darker shade every `ALT_PERIOD / 2` = 2 road units, tied to the road position
  (`u0 + uk·z` of the scanline), so the road visibly streams past. The shade is box-filtered over the depth
  the sample row covers (a steady middle shade where the stripes are thinner than a scanline) and its
  contrast fades as `1 / (1 + z / ALT_FADE)` (`ALT_FADE` = 60): full at the car, half at 60 units
  (`EXT_ROAD`: 10 % of colour 8 in colour 7; `EXT_SHLD`: 22 % darker; 8 levels each).
The rock faces, the drop-offs and the valley floor follow Test Drive Enhanced (`../TestDriveEnhanced`,
`cliff_face`, `left_side` and `valley_row` there), adapted to this renderer; its background mountains are not
used (the horizon stays the original's). Its depths are converted by the draw distances (its 40 / 120 rows
are 60 / 180 here) and its heights by the eye height (12 there, 80 here).

* **Rock faces.** The original draws a cliff as one colour-6 fill from its cut line to the edge of the view
  and everything above it, with its cliff-edge sprite (`lcfa` / `rcfa`) at the cut row. Here every pair of
  neighbouring cliff rows gets a face (`CMD_FACE`, `do_face`) above the outer road edge, leaning outwards by
  `CLIFF_LEAN` (0.2 px per px, the slant of that sprite), so the rock follows the road in bends and over
  hills. The rock is fixed in the world: each cliff unit has a height above its road edge (`cliff_rise`, not
  depending on the view) that is projected like the road (`height · ky / z` px), so a piece of rock only grows
  by perspective as the car approaches, and it rises and falls with the road. The height is at least
  `RIDGE_MIN` = 1650 height units (the eye is 80 above the road), which reaches the top of the view at the
  original's distance on level ground, where its fill covers everything above, plus up to `RIDGE_VAR` = 1000
  varying slowly along the road (smooth noise of the unit, periods of 37 and 11 units), so the ridge has a
  skyline. Its top edge is notched by a noise of the road position (periods of 3 and 1.2 units, at most
  `JAG_DEPTH` = 22 % of the height) and its slanted outline by a noise of the height above the road and the
  road position (`edge_jag`: `EDGE_JAG` = 60 lateral units deep, periods of 150 and 50 height units, faded in
  over `JAG_FOOT` = 40 above the road edge), both fixed to the world. Each scanline covers the face between the
  two rows' edge points: neighbouring pairs share their points and notches and join without gaps. Within
  the original's rows the faces are drawn together at the row whose edge reaches farthest into the view (the
  original's cut row, where it draws its fill): what is farther (a car behind the rock in a bend) stays hidden,
  what is nearer (cars, poles, the cliff decorations) is drawn over it, and the nearest face of a side also
  covers everything outwards of it, as the fill does. Beyond them each pair is drawn at its own row. Far
  away the ridge hides the mountains behind it; the faces fade out (dithered) over the last tenth of the drawn
  distance, so nothing appears at its end. With a cliff within the original's rows the sky is filled across
  the whole width (the original leaves the cliff's side to its fill) and, as in the original, no mountains
  are drawn. The colour 6 is hazed with distance (`EXT_ROCK`, 16 levels, dithered) as there: none up to about
  27 units, then growing to `HAZE_MAX` = 60 % towards the haze colour (a mix of the sky colour, white and
  light grey, like its pale haze) at the end of the view. The tunnel portals' colour-6 fills get the same haze
  for their row, and the hill around a far tunnel mouth is as high as the rock of its unit and fades out
  like it; that hill is drawn in as many slices as it is half-pixels high, so its sides slope smoothly.
* **Drop-offs.** Where the original shows its sky colour beside a drop-off (outside the outer edge,
  outside tunnels; in bends the original also fills its ground colour up to its sky cut), the ground pass
  draws a **ground strip** `VERGE_W` = 0.3 of the road's half-width wide beside the shoulder (`EXT_VERGE`:
  the ground colour of that side darkening towards the edge, hazed), so the valley never reaches the road,
  and beyond it the **valley floor**. Under each pair of drop-off rows (`CMD_DROP`, `do_drop`) a **dark rim**
  hangs straight down from the outer edge of the strip (`RIM_H` = 45 height units), then the **hillside**
  falls away outwards at 1:1 (`kx / ky` px per px) down to the valley floor, from its dark top colour into
  the hill colour over `HILL_GRAD` = 400 height units, its outline notched like the rock faces' (fixed to
  the world), both hazed like the rock faces. They only paint the drop-off side (`enh_void`: the void,
  the valley and other rims and hillsides), so the road and the strip in front of them stay, and nearer
  pairs are drawn later; on a straight road they stay under the road (seen edge-on), in bends they carry
  the far road over the valley.
* **Valley floor.** A plane `VALLEY_H` = 4000 height units below the eye (50 times the eye height, as
  there), far below the road, so a scanline `dy` below the horizon is at depth `VALLEY_H · ky / dy`. Its
  fields are two octaves of smooth value noise (periods of 173 and 53 road units, one road unit being 90
  lateral units) between three field colours, with woods (period 28) — the colours of Test Drive Enhanced
  as mixes of green (2) and brown (6), `EXT_VALLEY`, 8 textures by 8 hazes, dithered. They are fixed to the
  ground: the road position of the scanline's depth comes towards the car as it drives, and they pan with the
  mountains when it turns (`valley_shift`, the mountains' scroll). The contrast fades with distance
  (`exp(−z / 2000)`) and each octave where it is only a few output pixels large (no shimmer); the floor is
  hazed towards the haze colour (`1 − exp(−z / 3000)`, up to `VALLEY_HAZE` = 60 % at the horizon). The noise
  is evaluated once per output pixel. Above the horizon (the road climbing) the sky colour stays
  (`EXT_VOID`).
* **Wider scenery.** The view is wider than the original's and the sides were empty beyond the road's
  edge. A tree or shrub the original places (`scenery`) gets one or two more (`scenery_extras`) on the same
  side, `EXTRA_OUT` = 6 eighths of the road's half-width further out and `EXTRA_STEP` = 5 more for the
  second (plus 0–2 at random), and up to `EXTRA_DEPTH` = 0.45 units farther away (drawn between the row and
  the next one, before the placed one). Their kind is the placed one's or, at random, that of one of the
  four units before it (from the ring slot, or in the mirror from the scenery history, which holds the
  same). Everything is derived from a hash of the road unit, so they stay put, look the same in the mirror
  and the simulation, its ring and `mem[]` are untouched. Only trees and shrubs get them: sprites whose
  largest variant is at least 30 % green (colours 2 and 10; remembered in the decoded sprite), not the
  redwood trunks (the cut-off groups), not houses, rocks or signs; none beside a tunnel, a cliff or a
  drop-off on that side (this row or the next), and none reaching beyond the far-right band (water).

## Plan

Done: smooth 60 fps motion, 180-unit draw distance, 4× resolution, smooth turning (per-unit yaw and
lateral), view consistent at curvature changes, clean message boxes and prompts, high-resolution fall
view, and:

1. **Rear-view mirror** with the same renderer: smooth motion, higher resolution, 75 units behind. Done.
2. **Far tunnels and cliffs:** rock faces and tunnel entrances beyond 60 units grow into the original's
   fill without a step. Done.
3. **California stage 1 bug:** the mountains stay on the original's horizon. Done.
4. **Steering delay:** half a unit plus 110 ms of smoothing instead of 100 ms plus one unit — about a
   quarter shorter and measurably smoother. Done.

Next, the first **new-assets** stage, following Test Drive (1987) Enhanced (`../TestDriveEnhanced`),
colours beyond the 16 EGA ones where needed:

5. **Road markings:** thicker centre dashes and lane lines, scaled with the road width. Done.
6. **Road pattern:** alternating road and shoulder shades every few units, so speed is visible. Done.
7. **Rock faces:** the original's plain face with its slant and a notched outline, hazed with distance,
   continuous from the near wall to the far ridges (Test Drive Enhanced's faces for every cliff row). Done.
8. **Scenery below the road:** drop-offs get a ground strip beside the road, a dark rim, a hillside and a
   valley floor far below that moves as you drive (Test Drive Enhanced's), instead of flat colour. Done.
9. **Wider scenery:** extra trees and shrubs further out to the sides, next to the placed ones with a
   slight offset (derived deterministically from the ring slot, so the simulation is not affected; not
   the redwoods), because the wider view leaves the sides empty. Done.

Later: distance haze towards the horizon, a stage clock, higher-resolution sprites.

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
| `TD2_SNAPSHOT_HELD=1` | also save frames that stayed on the screen for over 150 ms as `heldNNNN.bmp` (messages and prompts are presented once and then held) |
| `TD2_KEYS="<s>:<xt>[+<xt>...],..."` | press and release XT keys (hex scan codes) that many seconds after start-up, e.g. `17:1d+19` = Ctrl-P; `p` after the codes only presses them (held), `r` only releases them |
| `TD2_ENH_LIVES=<n>` | lives in the attract mode |
| `TD2_ENH_EVENTS="<step>:<result>,..."` | sets the drive result that many simulation steps after the stage start (attract mode, also with `--classic`): 1 fill 'er up, 2 crash, 3 engine smoke, 4 out of gas, 5–8 damage messages, 9 too far left |
| `TD2_ENH_STAGE=<code><stage>` | the attract mode drives that stage (e.g. `CCC3`, `EC_0`) |
| `TD2_ENH_START=<unit>` | the attract mode starts that many units into the stage (also with `--classic`) |
| `TD2_ENH_COMPARE_DIR=<dir>`, `TD2_ENH_COMPARE_MS` | no extrapolation, whole units, no smoothing; every 2 s (or that many ms) `cmpNNNN.bmp` (enhanced window and mirror above the original's) and `cmpNNNN.txt` / `cmpNNNN_m.txt` (the view's and the mirror's rows, state, display list, and the original's rows of the same frame) |
| `TD2_ENH_STATS=1` | render / overlay times every 300 frames on stderr |
| `TD2_ENH_TRACE=<file>` | per-frame values: time, position, lateral, view yaw, scroll, screen x of the road centre 10 / 30 / 60 units ahead, a tracked car's id / screen x / distance, step position, render ms, camera heading drawn (road curve sum / 4 − view yaw) and the simulation's at its last step, steering angle, road curve, delayed read position, steering part of yaw |
| `TD2_ENH_DRIVER=follow` / `weave` / `lazy` / `offleft` / `offright` / `offwater` | the attract mode steers like a player (steering input and yaw integration instead of the demo's fixed yaw); `weave` changes lanes every 3 s, `lazy` only steers in 2 of 10 steps (steering held through bends); `offleft` / `offright` drive off the road (drop-offs, walls), `offwater` gets up to speed and then pushes the car right into a water zone |
| `TD2_ENH_LAG_MS`, `TD2_ENH_LAG_UNITS`, `TD2_ENH_LAG_TAU` | how far behind the car the steering part of the yaw and the lateral are read (0 ms, 0.5 units) and the smoothing time constant (110 ms) |
| `TD2_ENH_DEBUG=1` | sprite group sizes at stage start, and the stage's colours and unit ranges of its tunnels, cliffs and drop-offs |
