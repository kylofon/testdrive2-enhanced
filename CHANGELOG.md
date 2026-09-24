# Changelog

## 0.3.0 (2026-09-24)

### Launcher

- `TD2 Enhanced.exe`, the game's launcher (`launcher/`, built with `-DTD2_LAUNCHER=ON`).
- Quick start: Start a race with no intro, menus or difficulty screen. Parametres: `--start STAGE|default`, `--race clock|opponent`, `--car CODE`, `--opponent CODE`.

### Game

- Game speed (`--sim-ticks`, default 7, feels best so made default; "Game speed" in the launcher): `--classic` keeps the
  original's 10.

### Road view

- Enhanced road (`--enhanced-road`, on by default): the road's lighter and darker bands at Test Drive
  Enhanced's contrast instead of the barely visible shade before. `off` gives the
  original's plain road.
- Enhanced side scenery (`--enhanced-sides`, on by default): the ground beside the road alternates between
  its colour and a darker shade with the road's bands, as in Out Run (not the rock, mountains, water or sky).
- Mix cars between sceneries (`--mix-cars on`, off by default): the Beetle and the grey Saab from Europe also
  drive in California and the Master Scenery, and the Mercedes in Europe.

## 0.1.1 (2026-09-23)

- Head-to-head: the opponent in the rear-view mirror was too small. 

## 0.1.0 (2026-09-22)

- Project set up from the faithful Test Drive II port (TD2EGA.EXE).
- Enhanced road renderer: 60 fps smooth motion, 4× resolution with smoothed edges, 180-unit draw distance
  (`--draw-distance`, the original: 60), all road features, traffic, opponent and police; the cockpit,
  messages and prompts stay the original's. `--classic` shows the original renderer for comparison.
- Smoother steering and bends: the view follows the road evenly, without jerks or swings into bends, and
  follows your steering with a shorter delay.
- Rear-view mirror drawn by the same renderer, 75 units behind instead of 25.
- Roadside scenery appears 120 units ahead instead of 44; one or two more trees or shrubs further out to the
  sides.
- Rock faces with a notched outline and cracks, hazed with distance, from the near wall to the far ridges;
  steep rock faces below drop-offs with the sky below, as in the original (`--valley on`: a valley floor far
  below); far tunnel entrances set in their hill; fences where a drop-off ends.
- Falling off the road is seen from a camera that falls with the car.
- Thicker road markings and alternating road and shoulder shades.
- Cars and roadside objects at their true size at every distance, from their most detailed sprites
  (`--sprite-detail max`; `auto`: the size chosen by distance).
- Position indicator for reporting problems (F9 or `--show-position on`), e.g. `CCC0 790 X160`.
- Map viewer (`--viewer STAGE [--viewer-start UNIT]`) and its launcher, `TD2 Map Viewer.exe`: fly along any
  stage with the enhanced road view.
