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
- Options `--res-scale`, `--draw-distance`, `--classic`; `--frame-rate` defaults to 60.
- Developer aids: `TD2_ENH_STAGE`, `TD2_ENH_START`, `TD2_ENH_COMPARE_DIR`, `TD2_ENH_STATS`,
  `TD2_ENH_TRACE`, `TD2_ENH_DRIVER`, `TD2_ENH_DEBUG`, snapshot interval / start / count.
