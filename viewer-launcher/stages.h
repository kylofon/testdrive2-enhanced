// stages.h -- the sceneries and stages in a game folder, where the game's
// program is, and starting its map viewer.
#pragma once

#include <wx/string.h>

#include <vector>

// A scenery as SCENES.DAT lists it ("CCC Calif_Challenge 7"): its code, name
// and the stages whose data file (<code><stage>.DAT) is in the folder.
struct Scenery {
    wxString code;           // e.g. "CCC", "ec_"
    wxString name;           // e.g. "Calif Challenge"
    std::vector<int> stages;  // stage numbers 0.. present in the folder
};

// The sceneries of a game folder, in SCENES.DAT's order; empty if it has no
// SCENES.DAT or no stage of any scenery.
std::vector<Scenery> ReadSceneries(const wxString& gameDir);

// The stage code the viewer takes: the scenery code and the stage digit, e.g. "CCC0", "EC_5".
wxString StageCode(const Scenery& scenery, int stage);

// The folder the launcher runs from.
wxString LauncherDir();

// Where things are by default: "Game" and testdrive2-enhanced(.exe) beside the launcher.
wxString DefaultGameDir();
wxString DefaultProgram();

// Whether `dir` holds the file `name` (as written, or in upper or lower case).
bool FilePresent(const wxString& dir, const wxString& name);

struct ViewerOptions {
    wxString program;       // testdrive2-enhanced(.exe)
    wxString gameDir;       // --game-dir
    wxString stage;         // --viewer: e.g. "CCC0"
    int startUnit = 0;      // --viewer-start
    int scale = 3;          // --scale: the window is 320x240 times this
    int resScale = 4;       // --res-scale: the picture is 320x200 times this
    int drawDistance = 180; // --draw-distance
    bool showPosition = true;  // --show-position on / off
};

// Starts the viewer. On failure returns false and says why in `error`.
bool LaunchViewer(const ViewerOptions& options, wxString& error);
