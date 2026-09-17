# Test Drive II Enhanced — SDL3

An enhanced version of the EGA *Test Drive II: The Duel* (1989) by Accolade / Distinctive Software, running
natively on SDL3. The game itself is the faithful C reimplementation of `TD2EGA.EXE`, so driving, the
computer opponent, traffic, police and the game flow work as in the original. The view through the
windscreen is being redrawn with smooth motion and a longer draw distance (work in progress, see
`ENHANCED.md`). It is not an emulator - the original data is not redistributed, and you need to get it
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
| `--frame-rate FPS` | Drawing rate while driving (`0` = unpaced) |
| `--check` | Verify that `TD2EGA.EXE` loads, then exit without opening a window |

Alt+Enter toggles fullscreen. A connected gamepad acts as the joystick (Ctrl-J to calibrate / enable).

## Controls (from the original)

* Arrow keys / numeric keypad: steer, accelerate, brake; shifting as in the original.
* Esc: quit the current drive or menu.
* Ctrl-P pause, Ctrl-X exit to DOS, Ctrl-S sound, Ctrl-Q music, Ctrl-K keyboard, Ctrl-J joystick.

## Changes from original

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
