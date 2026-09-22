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
- Falling off the road is drawn by the enhanced renderer from a camera that falls with the car (in the
  water, the original's scrolled view).
- New assets, first stage: colours beyond the 16 EGA ones (mixes of the stage's colours that follow the
  palette); thicker centre dashes and lane lines scaled with the road width, anti-aliased and without
  flickering dashes in the distance.
- Road pattern: the road and shoulder shades alternate every two road units, so speed is visible; subtle and
  fading with distance.
- Rock faces and drop-offs after Test Drive Enhanced (without its background mountains): every cliff row
  gets the original's plain rock face with its slant and a notched outline, so the rock follows the road in
  bends, reaching the top of the view within the original's distance and settling towards the horizon
  beyond it, hazed with distance (tunnel portals too). Beside drop-offs a ground strip darkening towards the
  edge keeps the valley away from the road; below it a dark rim and a 1:1 hillside carry the far road in
  bends, and far below lies a valley floor with fields and woods that come towards the car and pan with the
  mountains, hazed towards the horizon. Far tunnel hills slope smoothly.
- Rock faces reach the top of the view at every distance (no skyline, no sky above the rock on the cliff
  side) and only grow by perspective as you approach; at the end of the drawn distance the rock recedes into
  the haze and the sky colour instead of a dithered curtain, far tunnel hills too. Their notched outline is
  fixed to the rock (also on hillsides).
- The cracks and tufts on the rock face stay on the rock in bends (they were drawn over the sky beside it),
  appear much further away (out to 120 units) and stay solid lines in the distance, fading with the haze;
  they are drawn only on their own face, not on a nearer or farther one.
- Drop-offs: the brown strip and dark rim between the road and the drop are gone; the drop is a steep rock
  face (twice the slant of the rock above), shaded darker. Test: `--valley off` (the default for now) shows
  the sky below the drop, as in the original, instead of the valley floor;
  `--valley on` brings the valley back.
- Car sprites as in Test Drive Enhanced: the most detailed size variant that is drawn at half its size or
  larger, so cars are mostly scaled down from a more detailed sprite; the smallest variant is no longer used.
- Cars keep their true size at every distance: the original's middle and far car sprites are up to 2.7 times
  too big for the distance they are drawn at (most visible with Europe's red VW Beetle and grey Saab, which
  swelled when they came from the distance and shrank when passing); every car is now scaled from its
  nearest (largest) sprite in proportion to the distance, in the front view and the mirror, 26.5 % larger (1.15 × 1.1).
- Test: `--sprite-detail max` (the default for now) draws every car and roadside object with its most
  detailed sprite at every distance, scaled to its true size, from reduced copies that keep small sprites from
  sparkling; `--sprite-detail auto` chooses the sprite size by distance as before. Tall scenery whose largest
  sprites are cropped at the top (windmills, a chalet, ruins) is drawn whole from its largest complete sprite;
  the redwood trunks keep the original's sprites.
- The gas station at the end of a stage is drawn at the original's size again, from a sprite with 3.5 times
  the pixels: its near sprites are a close-up of the pumps rather than the whole station, so taking the
  largest one for the object's size in the world drew the station at a third of its width, a smudge behind
  the trees. A group framed differently at different sizes now keeps the original's sizes and only takes a
  larger sprite of the same shape.
- Position indicator for reporting problems (on by default for now, F9 or `--show-position off` hides it):
  the scenery code and stage, the road unit and the lateral position in the corner of the road view, e.g.
  `CCC0 790 X160`, the values `TD2_ENH_STAGE` / `TD2_ENH_START` take to start there.
- Far tunnel entrances: the hill around the mouth covered the mouth below its top (a thin line of the mouth
  was left across the rock, e.g. TDS21 near unit 600); the mouth is now open, showing the tunnel and its far
  end, with its roof dark.
- Climbing towards a bend with the cliff on one side and the drop on the other: the rock of the bend ahead
  was cut off at the crest line with a band of sky below it (e.g. CCC0 1691); it now comes down beside the
  near road.
- In bends the rock face below the road no longer shows a see-through wedge of the far rock and sky (e.g.
  CCC0 2211): rock behind a crest had been drawn over it, and the nearer drop face now covers such rock.
- A bridge fence (like the Dutch bridges' walls) along the road before the end of the cliff road on
  California stage 1 (CCC0 units 2250-2432), covering the hole where the drop ends; render only. Placed
  from the road data wherever a drop-off ends in plain ground (also CCC4 1527-1709).
  Seen from its outer side in a bend it reaches down to its foot over the drop (it was clipped at the
  nearer road's top and floated above the rock).
- Rock of a far cliff seen behind a crest comes down to the drop side evenly: the farthest face, faded
  into the sky colour, no longer shows as a pale box below the rock beside a far fence (CCC0 2182).
- Cliff decorations whose mask and image differ in size stay on the rock too: in bends their cracks were
  drawn on the sky beside the face (CCC0 1278).
- The white start / finish band no longer shows as a line across the road climbing towards it: behind a
  crest its scanlines belong to the nearer road, and the band is now drawn only on its own road (CCC0
  approaching units 2527 / 2549).
- The hill of a tunnel entrance beside a drop-off stands straight up above the road, only notched, instead
  of leaning out over the drop like the rock faces (e.g. CCC3 1810); cliff sides and plain slopes unchanged.
- Tunnels (the first style) in bends: the inner wall hides what lies beyond the point where it turns out of
  sight. The far road, the tunnel's frames, posts and road markings showed through it (e.g. EC_2 3247, a post
  beside the exit at TDS23 935).
- A far tunnel near the end of the drawn distance: its mouth fades into the sky colour with the hill around it
  instead of a black hole in the faded rock, and the walls between its mouth and far end show only through the
  mouth (black blocks beside the hill over the drop side, e.g. TDS23 752).
- Cliffs beyond a sharp bend come into view with the turn, as the original's designers intended: a cliff
  whose start lies more than 30 degrees of turning ahead is not drawn yet and fades in as the turn left on the
  way to it drops to 20 degrees (or within 26 units). Every bend on the way counts, whichever way it goes, so
  the figure only falls as the car comes closer: a cliff cannot show before a bend, go again within it and
  come back after it (e.g. EC_2 1119). Its rock no longer shows across the inside of the bend over the
  sky long before (e.g. EC_5 340: sky while turning left, the cliff as the turn ends at 374, the other side's
  cliff with the right turn to 466).
- The cracks and tufts on the rock face are placed twice as high above the road edge as the original puts
  them (their own size unchanged), so the pattern shows further up the wall instead of in a band along the
  road.
- A pale ledge over the drop beside the road, with no road on top (e.g. TDS21 1082), is gone: behind a crest
  a drop face no longer paints the drop side above the crest's road line, where there is nothing for its rock
  to hang from. Below that line it still carries the rock of the bend ahead down beside the near road.
- Falling into the ravine is the real scene seen from a camera that falls: the eye drops below the road
  with the original's `fall_scroll`, so the road and its signs recede upwards, the rock face of the drop
  passes the camera and the far side of the ravine and the sky are the driving view's own, with the same
  colours and haze; the mirror falls with it. Timing, crash and message are the original's.
- No more sky showing through under the road while falling: seen from below the road folds back on itself
  and crosses a scanline several times, so the ground is drawn from every pair of rows, far to near,
  instead of one pair per scanline, and the rock of the drop meets the road's edge.
- The mountains fade out as soon as a cliff's rock comes out of the sky at the end of the view (gone when
  it is half there) instead of staying up above the terrain until the cliff reaches the original's rows.
- Tunnel entrances: the cliff beside the road runs on into the rock above the mouth (the hill takes the rock
  face's outline instead of a separate slope), and beside a drop-off the rock next to the mouth has the
  notched outline and goes on below the road as the drop face, far away and within the original's rows
  (instead of the original's straight edge and cliff-edge sprite), e.g. TDS21 near unit 700. The rock beside
  the mouth on a drop-off side stands out from the road edge and carries on below the road (CCC3 near
  unit 1830, TDS21 near 770); no line of ground colour across the sky beside it (the road beyond the
  tunnel, or the strip in front of the entrance).
- The CCC5 lighthouse is drawn whole (with --sprite-detail max its largest sprite, a close-up of the
  house without the tower, was taken for the whole object).
- Bridges (the second tunnel style) far away: their walls are no higher than 90 units above the road
  (the original's line just above the horizon of its rows stood far above a distant bridge, e.g. CCC5
  near unit 275).
- In tunnels: no line of the rock or ground beyond the far end across the wall at the horizon (the face at
  the far end is seen through the opening only; the ground outside the road inside a tunnel is its wall).
- No twitch of the road ahead entering bends and through S-bends: the road-curve part of the view's yaw is
  paired with the road integrator one unit on.
- Wider scenery: one or two more trees or shrubs further out beside the ones the original places (not the
  redwoods, houses or signs; not beside cliffs, drop-offs, tunnels or water), derived from the road
  position; render-only.
- Map viewer (`--viewer STAGE [--viewer-start UNIT]`): flies a camera along a stage with the enhanced road
  view filling the whole window (no cockpit, mirror, instruments or other cars, no simulation): Up / Down along
  the road, Left / Right to the sides (also far beyond the road), Shift ten times as fast, Page Up / Down 100
  units, Home / End, F9 position readout, Esc. The camera follows the road; the per-unit state (region, road
  object toggles, mountain scroll) and the roadside scenery (placed objects, and the random scenery by the
  simulation's rules with a hash of the unit) are derived from the road for any unit, so it can go back and
  jump. The view is 320 × 200 (70 rows more above the horizon, 38 below); the redwood trunks go on up to its
  top. Normal play is unchanged.
- `TD2 Map Viewer.exe` (`viewer-launcher/`, wxWidgets, `-DTD2_VIEWER_LAUNCHER=ON`): picks the game folder, the
  scenery and stage (read from `SCENES.DAT` and the stage files present), the start unit, window size,
  resolution, draw distance and position readout, and starts the viewer; it stays open for more stages.
- Options `--res-scale`, `--draw-distance`, `--classic`; `--frame-rate` defaults to 60.
- Developer aids: `TD2_ENH_STAGE`, `TD2_ENH_START`, `TD2_ENH_COMPARE_DIR` / `_MS`, `TD2_ENH_STATS`,
  `TD2_ENH_TRACE`, `TD2_ENH_DRIVER` (also driving off the road), `TD2_ENH_DEBUG`, `TD2_ENH_LIVES`,
  `TD2_ENH_EVENTS`, `TD2_KEYS`, `TD2_ENH_LAG_MS` / `_UNITS` / `_TAU`, snapshot interval / start / count /
  held frames.
