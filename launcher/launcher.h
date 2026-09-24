// launcher.h -- the launcher window: choose the game folder, how the game
// starts (the intro and menus, or a race at once on a chosen stage and car),
// the options of the enhanced view, then Play.
#pragma once

#include <wx/dialog.h>

#include <vector>

#include "game.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

extern const char* const APP_TITLE;

class LauncherDialog : public wxDialog {
public:
    LauncherDialog();

private:
    // Reads the folder's sceneries and cars again and refills their lists,
    // keeping the chosen ones where they still exist.
    void Reload();
    void FillStages();
    void FillCars();
    // Checks the folder and the program and enables Play and the controls to match.
    void UpdateState();
    void BrowseFolder();
    void BrowseProgram();
    void Play();
    void About();
    void Save();

    wxTextCtrl* folder_ = nullptr;
    wxTextCtrl* program_ = nullptr;
    wxStaticBitmap* statusIcon_ = nullptr;
    wxStaticText* statusNote_ = nullptr;
    wxCheckBox* quick_ = nullptr;
    wxChoice* scenery_ = nullptr;  // "Default", then the sceneries
    wxChoice* stage_ = nullptr;
    wxChoice* race_ = nullptr;     // the clock / the opponent
    wxChoice* car_ = nullptr;      // "Default", then the cars
    wxChoice* opponent_ = nullptr; // "Default", then the cars that can be raced against
    wxChoice* scale_ = nullptr;
    wxChoice* resScale_ = nullptr;
    wxSpinCtrl* distance_ = nullptr;
    wxSpinCtrl* speed_ = nullptr;     // --sim-ticks
    wxCheckBox* road_ = nullptr;
    wxCheckBox* sides_ = nullptr;
    wxCheckBox* mix_ = nullptr;
    wxCheckBox* valley_ = nullptr;
    wxCheckBox* detail_ = nullptr;
    wxCheckBox* position_ = nullptr;
    wxCheckBox* classic_ = nullptr;
    wxButton* play_ = nullptr;

    std::vector<Scenery> sceneries_;
    std::vector<Car> cars_;
    std::vector<Car> opponents_;
    // The choices, kept while the lists are refilled; empty codes are "Default".
    wxString sceneryCode_;
    int stageNumber_ = 0;
    wxString carCode_;
    wxString opponentCode_;
    bool loading_ = true;  // no edits are recorded while the window is built
};
