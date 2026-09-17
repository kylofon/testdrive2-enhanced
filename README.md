# Test Drive II Enhanced — SDL3

An enhanced version of the EGA *Test Drive II: The Duel* (1989) by Accolade / Distinctive Software, running
natively on SDL3. The game itself is the faithful C reimplementation of `TD2EGA.EXE`, so driving, the
computer opponent, traffic, police and the game flow work as in the original. The view through the
windscreen is redrawn at a higher resolution with smooth 60 fps motion and a longer draw distance, from
the original data only (see `ENHANCED.md`). It is not an emulator - the original data is not redistributed, and you need to get it
yourself (*Test Drive II: The Collection*, with the Supercars / Muscle Cars and California / European
Challenge add-ons, is supported).

## Requirements

* Your game files in a folder (by default `Game` under the working directory). The game needs
  `TD2EGA.EXE`, `CARS.DAT`, `SCENES.DAT`, `SONGS.BIN`, `VOICES.BIN`, the `*.PES` archives, the car
  `*.BIN` / `*.SS` files and the scenery `*.DAT` / `*.SGN` / `*.FNT` files. `select.dat` and the
  `*hisc.dat` high-score files are written to the same folder.
* CMake 3.24+, a C11 compiler and SDL 3.

## Build

From the repository root, in Git Bash or an MSYS2 MinGW64 shell:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

A `Makefile` wraps the same commands: `make build`, `make check`, `make run`, `make clean`, and
`make syntax` to compile-check the sources without linking. `make help` lists every target.

## Run

```bash
./build/testdrive2-enhanced.exe --game-dir Game
```

| Option | Meaning |
|---|---|
| `--game-dir DIR` | Folder with the original game files (default `Game`) |
| `--scale N` | Initial window size as a multiple of 320×240 (default 3) |
| `--res-scale N` | Output resolution as a multiple of 320×200 (default 4 = 1280×800, range 1–8; lower it on slower CPUs) |
| `--draw-distance N` | Road units drawn ahead (default 180, range 60–240; the original draws 60) |
| `--frame-rate FPS` | Drawing rate while driving (default 60, `0` = unpaced) |
| `--classic` | Original road renderer at the original 15 fps, for comparison |
| `--check` | Verify that `TD2EGA.EXE` loads, then exit without opening a window |

Alt+Enter toggles fullscreen. A connected gamepad acts as the joystick (Ctrl-J to calibrate / enable).

## Controls (from the original)

* Arrow keys / numeric keypad: steer, accelerate, brake; shifting as in the original.
* Esc: quit the current drive or menu.
* Ctrl-P pause, Ctrl-X exit to DOS, Ctrl-S sound, Ctrl-Q music, Ctrl-K keyboard, Ctrl-J joystick.

## Changes from original

### Road view

* **Resolution:** the road is drawn at 4× the original resolution by default, with smoothed edges, in the
  original 16 colours. The cockpit, mirror and sprites keep their original pixel art, scaled up.
* **Smooth motion:** 60 fps instead of about 15. The road, the scenery and the other cars move
  continuously instead of one road unit at a time. Steering and bends turn the view evenly (the
  original turned it in whole-degree steps); the view follows your steering about 0.14–0.18 s behind
  (bends follow the road with no delay at all).
* **Draw distance:** 180 road units instead of 60 (`--draw-distance`). Road signs, traffic, the opponent
  and the police are drawn that far; roadside scenery and text signs appear 120 units ahead instead of 44
  (the simulation places them 120 units ahead instead of 70, so the random scenery differs from the
  original's). Distant objects fade in.
* **Rear-view mirror:** drawn by the same renderer — smooth, at the same resolution, and 75 road units
  behind instead of 25. Everything the original's mirror does differently (sign masks, swapped car
  views, its own mountains, no clouds) is kept.
* **Objects:** signs, poles, scenery and cars change size smoothly with distance, matching the original's
  sizes where it drew them, and switch between the original's size variants without jumps. Cars are
  placed at their exact position on the road instead of the nearest road unit.
* **Road markings:** the centre line and lane lines are drawn as continuous dashes instead of one dot
  per road unit.
* **Cliffs and tunnels:** beyond the original's 60 units, rock faces and tunnel entrances are drawn as a
  rock mass of their own that grows into the original's wall and portal without a step, and a second
  tunnel in view is drawn behind the first.
* **Horizon:** the mountains stay where the original puts them when the road climbs in the distance.
* **Falling off the road:** the view scrolls up smoothly at the output resolution, with the original's
  sky, cliff and water fills below it; the mirror keeps its original image.
* **Kept from the original:** the mirror, dashboard, speeding ticket, messages and prompts (exactly as
  the original draws them, with no enhanced pixels inside), windscreen cracks, engine smoke, crash flash
  and GAME OVER.
* **Timing:** the gear-gate close delay keeps the original 15 fps timing.

### Game (from the faithful port)

* **Removed:** copy protection and all disk handling (disk prompts, `DISKID.DAT`, Play Disk
  creation; the Install menu icon does nothing), and the CGA / Tandy / Hercules modes.
* **Joystick:** a gamepad replaces the analogue PC joystick.

## Layout

* `ENGINE.md` — architecture and the rules the faithful engine follows.
* `ENHANCED.md` — design of the enhanced renderer.
* `src/enhanced/` — the enhanced renderer.
* `src/mem.*` emulates the real-mode address space the game ran in; `src/host.*` wraps SDL3.
* `src/platform/` — EGA graphics, resources, timer, sound, input and prompts.
* `src/game/` — game flow, scene rendering and simulation.

Reverse-engineering tools, specs and file formats are in the Test Drive II reverse-engineering
repository.

## License

MIT, see [LICENSE](LICENSE). *Test Drive II* and its data belong to their respective owners.

## Support

https://buymeacoffee.com/krzysztofkania
