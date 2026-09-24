# TD2 Enhanced (the launcher)

`TD2 Enhanced.exe` starts Test Drive II Enhanced (`testdrive2-enhanced`) with the options chosen in its window:
from the game's intro and menus as usual, or straight into a race on a stage and car picked here. It is built with
[wxWidgets](https://www.wxwidgets.org/) 3.2 from the platform's own controls, like the map viewer's launcher
(`viewer-launcher/`) it is modelled on.

## The window

* **Game files**
  * **Folder**: the folder with the original game's files (default `Game` beside the launcher).
  * **Program**: `testdrive2-enhanced.exe` (default: beside the launcher).
  * The line below says how many sceneries, stages and cars the folder has, or what is missing. **Play** stays
    greyed out until the folder and the program are there.
* **Start**
  * **Start a race at once**: off (the default), the game starts with its intro and menus. On, it goes straight
    onto the road (`--start`), without the intro, the menus or the difficulty screen (the difficulty the screen
    starts at); after the race the game goes on as usual: the results, the next stage, the menus.
  * **Scenery** and **Stage**: the stage the race starts on. *Default* is the scenery last chosen in the game's
    menu, its first stage.
  * **Race against**: the clock or the opponent (`--race`).
  * **Car** and **Opponent**: the cars (`--car`, `--opponent`), as if chosen in the game's menu, so they also
    apply when the game starts with its menus. *Default* is the game's own last choice. The opponent list only
    has the cars that can be raced against.
* **Options**
  * **Window size** (`--scale`), **Resolution** (`--res-scale`) and **Draw distance** (`--draw-distance`).
  * **Game speed** (`--sim-ticks`, default 6): timer ticks per simulation step; the original steps every 10.
    Fewer ticks make everything faster (your car, the opponent, the traffic, the police); the race clock always
    counts real seconds. Stored as `GameSpeed`.
  * **Enhanced road** (`--enhanced-road`, on by default): the road and its shoulders alternate between a lighter
    and a darker shade every two road units, as in Test Drive Enhanced, so the speed shows.
  * **Enhanced side scenery** (`--enhanced-sides`, on by default): the ground beside the road (grass, sand, earth)
    alternates between a lighter and a darker shade with the road's bands, as in Out Run. Rock faces, mountains,
    water and the sky stay plain.
  * **Mix cars between sceneries** (`--mix-cars`, off by default): some of the traffic is drawn as the other
    sceneries' cars - the Beetle and the grey Saab in California and the Master Scenery, the Mercedes in Europe -
    always keeping at least one of each of the scenery's own cars on a stage.
  * **Valley floor far below drop-offs** (`--valley`), **Most detailed sprites** (`--sprite-detail max`) and
    **Show the position** (`--show-position`; F9 in the game).
  * **Classic** (`--classic`): the original road view at 15 frames a second; the options above do not apply.
* **Keys in the game**: a reminder of the game's keys.
* **Play** starts the game; the launcher stays open. **About**: version, author and links.

Everything is remembered in `%APPDATA%\TD2 Enhanced\settings.ini` (`~/.config/td2-enhanced` on Linux). A folder
or program left at its default is stored empty, so it follows the launcher if the whole folder moves. Delete the
file to go back to the defaults.

## Building

Needs CMake 3.24, a C++17 compiler and wxWidgets 3.2 (MSYS2 `mingw64`: `mingw-w64-x86_64-wxwidgets3.2-msw`;
Debian and Ubuntu: `libwxgtk3.2-dev`). To build it beside `testdrive2-enhanced.exe`, add `-DTD2_LAUNCHER=ON` when
configuring the game:

```bash
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -DTD2_LAUNCHER=ON
cmake --build build
```

It also builds on its own (`cmake -S launcher -B launcher/build -G Ninja`); then choose the program in the window
or copy the launcher next to it.

On Windows the build copies every DLL the launcher needs beside it (`copy_dlls.cmake`): the two wxWidgets DLLs
and the MSYS2 libraries they load. Keep them with the `.exe` in a release. The C++ runtime of the launcher itself
is linked in.

## Files

* `app.cpp`: the wxWidgets application.
* `launcher.h`, `launcher.cpp`: the window and the About box.
* `game.h`, `game.cpp`: reading `SCENES.DAT` and `CARS.DAT`, the file checks and starting the game.
* `settings.h`, `settings.cpp`: `settings.ini`.
* `icon.h`, `icon.cpp`: the app icon, a banded road running to the horizon drawn in code. `make_icon.py` (Pillow)
  writes the same drawing to `app.ico` for Explorer.
* `app.rc`, `app.manifest`, `app.ico`, `version.h`: icon, visual styles, DPI awareness, version info.
* `copy_dlls.cmake`: the post-build DLL copy.
* `CMakeLists.txt`: the build, standalone or from the main project.
