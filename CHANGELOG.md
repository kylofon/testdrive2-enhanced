# Changelog

## Unreleased

- Project set up from the faithful Test Drive II port (TD2EGA.EXE).
- Enhanced road renderer: 60 fps motion with extrapolated simulation state, continuous-depth projection
  with the original integrators, 180-unit draw distance, 4× output resolution with supersampling in the
  original palette, all road features (drop-offs, cliffs, both tunnel styles, wide road, bands, road
  objects, scenery and text signs, traffic, opponent, police), EGA overlays kept.
- Scenery ring filled 120 units ahead (look-ahead region, zone test, density and placed objects moved
  with it).
- Smooth sideways motion: view yaw and lateral sampled per road unit and read one step and one unit
  behind (no prediction jerks when steering or in bends); `tan256` interpolated within the degree so
  bends turn the view evenly; other cars' laterals interpolated.
- No more view swing into bends without steering: only the steering part of yaw is delayed, the road
  curve part follows the road.
- Redwood trunks (sprites cut off by the top of the view) only within the original's scenery distance;
  far cliffs drawn as ridges along the road edge.
- Rear-view mirror drawn by the enhanced renderer: smooth, high resolution, 75 units behind instead of
  25, with all the original mirror's differences kept; the scenery behind the car comes from a per-unit
  history because the scenery ring is filled further ahead.
- Rock faces and tunnel entrances beyond the original's 60 units are drawn as a leaning wall along the
  road edge (and a hill around the mouth) whose height grows into the original's wall and portal without a
  step, and settles towards the horizon in the distance instead of standing over it.
- The mountains stay on the original's horizon when the road climbs in the distance (California stage 1).
- Shorter steering delay: half a road unit plus 110 ms of smoothing instead of 100 ms plus one unit
  (181 / 153 / 138 ms at 60 / 100 / 150 mph instead of 242 / 185 / 157 ms), with less frame-to-frame jerk.
- With `--classic` the scenery ring is the original's 70 units again.
- Message boxes, prompts and the joystick calibration screen no longer show enhanced road pixels where
  the road image was already black: the EGA model marks every pixel written since the road was
  presented; prompts restore that mark with the pixels they restore.
- Falling off the road is drawn by the enhanced renderer: the view scrolls up smoothly with the
  original's fills below it (drop left, drop right, water).
- New assets, first stage: colours beyond the 16 EGA ones (mixes of the stage's colours that follow the
  palette); thicker centre dashes and lane lines scaled with the road width, anti-aliased and without
  flickering dashes in the distance.
- Road pattern: the road and shoulder shades alternate every two road units, so speed is visible; subtle and
  fading with distance.
- Options `--res-scale`, `--draw-distance`, `--classic`; `--frame-rate` defaults to 60.
- Developer aids: `TD2_ENH_STAGE`, `TD2_ENH_START`, `TD2_ENH_COMPARE_DIR` / `_MS`, `TD2_ENH_STATS`,
  `TD2_ENH_TRACE`, `TD2_ENH_DRIVER` (also driving off the road), `TD2_ENH_DEBUG`, `TD2_ENH_LIVES`,
  `TD2_ENH_EVENTS`, `TD2_KEYS`, `TD2_ENH_LAG_MS` / `_UNITS` / `_TAU`, snapshot interval / start / count /
  held frames.
