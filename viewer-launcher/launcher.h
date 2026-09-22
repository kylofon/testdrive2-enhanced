// launcher.h -- the launcher window: choose the game folder, a scenery and
// stage, where to start and the options, then View. The window stays open, so
// several stages can be opened one after another.
#pragma once

#include <wx/dialog.h>

#include <vector>

#include "stages.h"

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
    // Reads the folder's sceneries again and refills the scenery and stage
    // lists, keeping the chosen stage where it still exists.
    void Reload();
    void FillStages();
    // Checks the folder and the program and enables View to match.
    void UpdateState();
    void BrowseFolder();
    void BrowseProgram();
    void View();
    void About();
    void Save();

    wxTextCtrl* folder_ = nullptr;
    wxTextCtrl* program_ = nullptr;
    wxStaticBitmap* statusIcon_ = nullptr;
    wxStaticText* statusNote_ = nullptr;
    wxChoice* scenery_ = nullptr;
    wxChoice* stage_ = nullptr;
    wxSpinCtrl* start_ = nullptr;
    wxChoice* scale_ = nullptr;
    wxChoice* resScale_ = nullptr;
    wxSpinCtrl* distance_ = nullptr;
    wxCheckBox* position_ = nullptr;
    wxButton* view_ = nullptr;

    std::vector<Scenery> sceneries_;
    wxString sceneryCode_;  // the chosen scenery and stage, kept while the lists are refilled
    int stageNumber_ = 0;
    bool loading_ = true;   // no edits are recorded while the window is built
};
