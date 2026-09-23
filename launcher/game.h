// game.h -- what a game folder holds (the sceneries and stages of SCENES.DAT,
// the cars of CARS.DAT), where the game's program is, and starting it.
#pragma once

#include <wx/string.h>

#include <vector>

// A scenery as SCENES.DAT lists it ("CCC Calif_Challenge 7"): its code, name
// and the stages whose data file (<code><stage>.DAT) is in the folder.
struct Scenery {
    wxString code;            // e.g. "CCC", "ec_"
    wxString name;            // e.g. "Calif Challenge"
    std::vector<int> stages;  // stage numbers 0.. present in the folder
};

// A car as CARS.DAT lists it ("F40 Ferrari_F40"), with its files in the folder.
struct Car {
    wxString code;          // e.g. "F40"
    wxString name;          // e.g. "Ferrari F40"
    bool opponent = false;  // its opponent's files (<code>O.BIN) are there too
};

// The sceneries of a game folder, in SCENES.DAT's order; empty if it has no
// SCENES.DAT or no stage of any scenery.
std::vector<Scenery> ReadSceneries(const wxString& gameDir);

// The cars of a game folder whose files are there, in CARS.DAT's order.
std::vector<Car> ReadCars(const wxString& gameDir);

// The stage code the game takes: the scenery code and the stage digit, e.g. "CCC0", "EC_5".
wxString StageCode(const Scenery& scenery, int stage);

// The folder the launcher runs from.
wxString LauncherDir();

// Where things are by default: "Game" and testdrive2-enhanced(.exe) beside the launcher.
wxString DefaultGameDir();
wxString DefaultProgram();

// Whether `dir` holds the file `name` (as written, or in upper or lower case).
bool FilePresent(const wxString& dir, const wxString& name);

struct GameOptions {
    wxString program;           // testdrive2-enhanced(.exe)
    wxString gameDir;           // --game-dir
    // Start: an empty stage plays the game from its intro; otherwise --start with this
    // code (or "default": the game's own scenery, its first stage).
    wxString startStage;
    bool againstOpponent = false;  // --race opponent / clock
    wxString car;               // --car: empty = the game's own choice
    wxString opponent;          // --opponent: empty = the game's own choice
    int scale = 3;              // --scale: the window is 320x240 times this
    int resScale = 4;           // --res-scale: the picture is 320x200 times this
    int drawDistance = 180;     // --draw-distance
    bool enhancedRoad = true;   // --enhanced-road on / off
    bool enhancedSides = true;  // --enhanced-sides on / off
    bool valley = false;        // --valley on / off
    bool detailMax = true;      // --sprite-detail max / auto
    bool showPosition = false;  // --show-position on / off
    bool classic = false;       // --classic
};

// Starts the game. On failure returns false and says why in `error`.
bool LaunchGame(const GameOptions& options, wxString& error);
