# TD2 Map Viewer

`TD2 Map Viewer.exe` starts the map viewer of Test Drive II Enhanced (`testdrive2-enhanced --viewer`) on a
stage you pick: a camera flies along the road with the enhanced road view filling the window, without the
cockpit, other cars or the game (see "Map viewer" in the main `README.md`). It is built with
[wxWidgets](https://www.wxwidgets.org/) 3.2 from the platform's own controls, like the Test Drive (1987)
launcher it is modelled on.

## The window

* **Game files**
  * **Folder**: the folder with the original game's files (default `Game` beside the launcher).
  * **Program**: `testdrive2-enhanced.exe` (default: beside the launcher).
  * The line below says how many sceneries and stages the folder has, or what is missing. **View** stays greyed
    out until the folder and the program are there.
* **Stage**
  * **Scenery**: the sceneries `SCENES.DAT` lists, e.g. *Calif Challenge (CCC)*, *European Challenge (EC_)*,
    *Master Scenery (TDS2)* (the original game's), with only the stages whose `.DAT` file is in the folder.
  * **Stage**: *Stage 1 (CCC0)* and so on: the code the viewer takes (`--viewer CCC0`) and the position readout
    shows.
  * **Start at unit**: the road unit to start at (`--viewer-start`), as the position readout counts it.
* **Options**: **Window size** (`--scale`, 320 × 240 to 1920 × 1440), **Resolution** (`--res-scale`, the picture
  320 × 200 to 2560 × 1600), **Draw distance** (`--draw-distance`, 60–240 road units) and **Show the position**
  (`--show-position`; F9 in the viewer).
* **Keys in the viewer**: a reminder of the viewer's keys.
* **View** starts the viewer with those settings; the launcher stays open, so several stages can be viewed side
  by side. **About**: version, author and links.

Everything is remembered in `%APPDATA%\TD2 Map Viewer\settings.ini` (`~/.config/td2-map-viewer` on Linux). A
folder or program left at its default is stored empty, so it follows the launcher if the whole folder moves.
Delete the file to go back to the defaults.

## Building

Needs CMake 3.24, a C++17 compiler and wxWidgets 3.2 (MSYS2 `mingw64`: `mingw-w64-x86_64-wxwidgets3.2-msw`;
Debian and Ubuntu: `libwxgtk3.2-dev`). To build it beside `testdrive2-enhanced.exe`, add
`-DTD2_VIEWER_LAUNCHER=ON` when configuring the game:

```bash
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -DTD2_VIEWER_LAUNCHER=ON
cmake --build build
```

It also builds on its own (`cmake -S viewer-launcher -B viewer-launcher/build -G Ninja`); then choose the program
in the window or copy the launcher next to it.

On Windows the build copies every DLL the launcher needs beside it (`copy_dlls.cmake`): the two wxWidgets DLLs
and the MSYS2 libraries they load (libstdc++, libpng, libtiff, ...). Keep them with the `.exe` in a release. The
C++ runtime of the launcher itself is linked in.

## Files

* `app.cpp`: the wxWidgets application.
* `launcher.h`, `launcher.cpp`: the window and the About box.
* `stages.h`, `stages.cpp`: reading `SCENES.DAT`, the file checks and starting the viewer.
* `settings.h`, `settings.cpp`: `settings.ini`.
* `icon.h`, `icon.cpp`: the app icon, a road running to the horizon drawn in code. `make_icon.py` (Pillow) writes
  the same drawing to `app.ico` for Explorer.
* `app.rc`, `app.manifest`, `app.ico`, `version.h`: icon, visual styles, DPI awareness, version info.
* `copy_dlls.cmake`: the post-build DLL copy.
* `CMakeLists.txt`: the build, standalone or from the main project.
