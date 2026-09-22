// launcher.cpp -- the launcher window.
#include "launcher.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#include <shellapi.h>
#endif

#include "icon.h"
#include "settings.h"
#include "version.h"

const char* const APP_TITLE = "TD2 Map Viewer";

namespace {

const char* const WEBSITE = "https://kkania.com";
const char* const SUPPORT = "https://buymeacoffee.com/krzysztofkania";

const int MIN_SCALE = 1, MAX_SCALE = 6, DEFAULT_SCALE = 3;
const int MIN_RES = 1, MAX_RES = 8, DEFAULT_RES = 4;
const int MIN_DISTANCE = 60, MAX_DISTANCE = 240, DEFAULT_DISTANCE = 180;
const int MAX_START = 9999;

#ifdef __WXMSW__
HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}
#else
// A label and a link on one line, for the portable About box.
void AddLink(wxWindow* parent, wxSizer* sizer, const wxString& label, const wxString& text, const wxString& url) {
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new wxStaticText(parent, wxID_ANY, label + " "), 0, wxALIGN_CENTER_VERTICAL);
    line->Add(new wxHyperlinkCtrl(parent, wxID_ANY, text, url), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line);
}
#endif

wxStaticText* GreyText(wxWindow* parent, const wxString& text = wxEmptyString) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    return label;
}

// A label, a text field and a Browse button in a row of a two-column grid.
wxTextCtrl* PathRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxString& tip,
                    wxButton** browse) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* text = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(parent->FromDIP(300), -1));
    text->SetToolTip(tip);
    *browse = new wxButton(parent, wxID_ANY, "B&rowse...");
    row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(8));
    row->Add(*browse, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(row, 1, wxEXPAND);
    return text;
}

}  // namespace

LauncherDialog::LauncherDialog()
    : wxDialog(nullptr, wxID_ANY, APP_TITLE, wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxMINIMIZE_BOX) {
    SetIcons(AppIcons());
    const int margin = FromDIP(12), gap = FromDIP(8), small = FromDIP(4);

    // Game files
    auto* filesBox = new wxStaticBoxSizer(wxVERTICAL, this, "Game files");
    wxWindow* fb = filesBox->GetStaticBox();
    auto* filesGrid = new wxFlexGridSizer(2, gap, gap);
    filesGrid->AddGrowableCol(1);
    wxButton* browseFolder = nullptr;
    wxButton* browseProgram = nullptr;
    folder_ = PathRow(fb, filesGrid, "&Folder:", "The folder with the original game's files.", &browseFolder);
    program_ = PathRow(fb, filesGrid, "&Program:",
                       "testdrive2-enhanced, the game whose map viewer shows the stage.", &browseProgram);
    auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
    statusIcon_ = new wxStaticBitmap(fb, wxID_ANY, wxArtProvider::GetBitmapBundle(wxART_WARNING, wxART_MENU));
    statusNote_ = new wxStaticText(fb, wxID_ANY, wxEmptyString);
    statusRow->Add(statusIcon_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, small);
    statusRow->Add(statusNote_, 1, wxALIGN_CENTER_VERTICAL);
    filesBox->Add(filesGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);
    filesBox->Add(statusRow, 0, wxEXPAND | wxALL, gap);
    folder_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!loading_) Reload();
    });
    program_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!loading_) UpdateState();
    });
    browseFolder->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseFolder(); });
    browseProgram->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseProgram(); });

    // Stage
    auto* stageBox = new wxStaticBoxSizer(wxVERTICAL, this, "Stage");
    wxWindow* sb = stageBox->GetStaticBox();
    auto* stageGrid = new wxFlexGridSizer(2, gap, gap);
    stageGrid->Add(new wxStaticText(sb, wxID_ANY, "S&cenery:"), 0, wxALIGN_CENTER_VERTICAL);
    scenery_ = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(220), -1));
    stageGrid->Add(scenery_, 0, wxALIGN_CENTER_VERTICAL);
    stageGrid->Add(new wxStaticText(sb, wxID_ANY, "&Stage:"), 0, wxALIGN_CENTER_VERTICAL);
    stage_ = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(220), -1));
    stageGrid->Add(stage_, 0, wxALIGN_CENTER_VERTICAL);
    stageGrid->Add(new wxStaticText(sb, wxID_ANY, "Start at &unit:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* startRow = new wxBoxSizer(wxHORIZONTAL);
    start_ = new wxSpinCtrl(sb, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1),
                            wxSP_ARROW_KEYS, 0, MAX_START, 0);
    start_->SetToolTip("The road unit to start at, as the position in the corner shows it (e.g. 790 in "
                       "\"CCC0 790 X160\"). Past the end of the stage: its end.");
    startRow->Add(start_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    startRow->Add(GreyText(sb, "road units from the start"), 0, wxALIGN_CENTER_VERTICAL);
    stageGrid->Add(startRow, 0, wxALIGN_CENTER_VERTICAL);
    stageBox->Add(stageGrid, 0, wxALL, gap);
    scenery_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = scenery_->GetSelection();
        if (i >= 0) sceneryCode_ = sceneries_[i].code;
        FillStages();
    });
    stage_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = scenery_->GetSelection(), k = stage_->GetSelection();
        if (i >= 0 && k >= 0) stageNumber_ = sceneries_[i].stages[k];
    });

    // Options
    auto* optionsBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
    wxWindow* ob = optionsBox->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, gap, gap);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Window size:"), 0, wxALIGN_CENTER_VERTICAL);
    scale_ = new wxChoice(ob, wxID_ANY);
    for (int s = MIN_SCALE; s <= MAX_SCALE; ++s) scale_->Append(wxString::Format(L"%d × %d", 320 * s, 240 * s));
    scale_->SetToolTip("The window's size when the viewer starts. Alt+Enter switches to full screen.");
    grid->Add(scale_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Resolution:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* resRow = new wxBoxSizer(wxHORIZONTAL);
    resScale_ = new wxChoice(ob, wxID_ANY);
    for (int s = MIN_RES; s <= MAX_RES; ++s) resScale_->Append(wxString::Format(L"%d × %d", 320 * s, 200 * s));
    resScale_->SetToolTip("The resolution the picture is drawn at. Lower it on a slower computer.");
    resRow->Add(resScale_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    resRow->Add(GreyText(ob, "the original: 320 × 200"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(resRow, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Draw distance:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* distRow = new wxBoxSizer(wxHORIZONTAL);
    distance_ = new wxSpinCtrl(ob, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1),
                               wxSP_ARROW_KEYS, MIN_DISTANCE, MAX_DISTANCE, DEFAULT_DISTANCE);
    distRow->Add(distance_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    distRow->Add(GreyText(ob, "road units ahead: 60 as in the original, up to 240"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(distRow, 0, wxALIGN_CENTER_VERTICAL);
    optionsBox->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    position_ = new wxCheckBox(ob, wxID_ANY, "Show the &position (F9 switches it on and off)");
    position_->SetToolTip("The stage, road unit and lateral position in the top left corner, e.g. "
                          "\"CCC0 790 X160\", to quote when reporting a place.");
    optionsBox->Add(position_, 0, wxALL, gap);

    // Controls
    auto* keysBox = new wxStaticBoxSizer(wxVERTICAL, this, "Keys in the viewer");
    wxWindow* kb = keysBox->GetStaticBox();
    auto* keys = new wxFlexGridSizer(2, small, FromDIP(16));
    const char* const KEYS[][2] = {
        {"Up / Down", "forward / back along the road"},
        {"Left / Right", "to the sides, also far beyond the road's edges"},
        {"Shift", "with the arrows: ten times as fast"},
        {"Page Up / Page Down", "100 road units on / back"},
        {"Home / End", "back to the start / the end of the stage"},
        {"F9", "position on / off"},
        {"Alt+Enter", "full screen"},
        {"Esc", "close the viewer"},
    };
    for (const auto& k : KEYS) {
        keys->Add(new wxStaticText(kb, wxID_ANY, k[0]));
        keys->Add(GreyText(kb, k[1]));
    }
    keysBox->Add(keys, 0, wxALL, gap);

    // Buttons
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* about = new wxButton(this, wxID_ABOUT, "&About");
    view_ = new wxButton(this, wxID_ANY, "&View");
    auto* close = new wxButton(this, wxID_CLOSE, "Close");
    buttons->Add(about);
    buttons->AddStretchSpacer();
    buttons->Add(view_, 0, wxRIGHT, gap);
    buttons->Add(close);
    view_->SetDefault();
    SetEscapeId(wxID_CLOSE);
    about->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { About(); });
    view_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { View(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(filesBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(stageBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(optionsBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(keysBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    SetSizer(all);

    // Settings from the last run.
    wxString dir = settings::GetString("Viewer", "GameFolder", "");
    wxString program = settings::GetString("Viewer", "Program", "");
    folder_->ChangeValue(dir.empty() ? DefaultGameDir() : dir);
    program_->ChangeValue(program.empty() ? DefaultProgram() : program);
    sceneryCode_ = settings::GetString("Viewer", "Scenery", "");
    stageNumber_ = settings::GetInt("Viewer", "Stage", 0);
    start_->SetValue(wxMax(0, wxMin(MAX_START, settings::GetInt("Viewer", "StartUnit", 0))));
    scale_->SetSelection(
        wxMax(MIN_SCALE, wxMin(MAX_SCALE, settings::GetInt("Viewer", "Scale", DEFAULT_SCALE))) - MIN_SCALE);
    resScale_->SetSelection(wxMax(MIN_RES, wxMin(MAX_RES, settings::GetInt("Viewer", "ResScale", DEFAULT_RES))) - MIN_RES);
    distance_->SetValue(settings::GetInt("Viewer", "DrawDistance", DEFAULT_DISTANCE));
    position_->SetValue(settings::GetInt("Viewer", "ShowPosition", 1) != 0);
    loading_ = false;
    Reload();

    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (event.GetActive()) Reload();  // files may have been copied in meanwhile
        event.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        Save();
        Destroy();
    });

    Fit();
    if (!settings::RestoreWindowPosition("Viewer", this)) Centre();
}

void LauncherDialog::Reload() {
    sceneries_ = ReadSceneries(folder_->GetValue());
    scenery_->Clear();
    int pick = 0;
    for (size_t i = 0; i < sceneries_.size(); ++i) {
        scenery_->Append(wxString::Format("%s (%s)", sceneries_[i].name, sceneries_[i].code.Upper()));
        if (sceneries_[i].code.CmpNoCase(sceneryCode_) == 0) pick = static_cast<int>(i);
    }
    if (!sceneries_.empty()) {
        scenery_->SetSelection(pick);
        sceneryCode_ = sceneries_[pick].code;
    }
    FillStages();
}

void LauncherDialog::FillStages() {
    stage_->Clear();
    const int i = scenery_->GetSelection();
    if (i >= 0) {
        const Scenery& s = sceneries_[i];
        int pick = 0;
        for (size_t k = 0; k < s.stages.size(); ++k) {
            stage_->Append(wxString::Format("Stage %d (%s)", s.stages[k] + 1, StageCode(s, s.stages[k])));
            if (s.stages[k] == stageNumber_) pick = static_cast<int>(k);
        }
        stage_->SetSelection(pick);
        stageNumber_ = s.stages[pick];
    }
    UpdateState();
}

void LauncherDialog::UpdateState() {
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    const bool haveProgram = wxFileName::FileExists(program);
    wxString note;
    if (!FilePresent(dir, "SCENES.DAT"))
        note = "This folder needs the game's files: SCENES.DAT, TD2EGA.EXE and the rest.";
    else if (sceneries_.empty())
        note = "SCENES.DAT lists no scenery whose stage files are in this folder.";
    else if (!FilePresent(dir, "TD2EGA.EXE"))
        note = "This folder needs TD2EGA.EXE.";
    else if (!haveProgram)
        note = wxString::Format("%s isn't there.", wxFileName(program).GetFullName());
    else {
        int stages = 0;
        for (const Scenery& s : sceneries_) stages += static_cast<int>(s.stages.size());
        note = wxString::Format("Found %d %s with %d stages.", static_cast<int>(sceneries_.size()),
                                sceneries_.size() == 1 ? "scenery" : "sceneries", stages);
    }
    const bool ok = haveProgram && !sceneries_.empty() && FilePresent(dir, "TD2EGA.EXE");
    statusIcon_->Show(!ok);
    statusNote_->SetLabel(note);
    scenery_->Enable(!sceneries_.empty());
    stage_->Enable(!sceneries_.empty());
    view_->Enable(ok && scenery_->GetSelection() >= 0 && stage_->GetSelection() >= 0);
    Layout();
}

void LauncherDialog::BrowseFolder() {
    wxDirDialog dialog(this, "Choose the folder with the game's files", folder_->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) folder_->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

void LauncherDialog::BrowseProgram() {
    wxFileName current(program_->GetValue());
#ifdef __WXMSW__
    const char* const filter = "Programs (*.exe)|*.exe|All files (*.*)|*.*";
#else
    const char* const filter = "All files|*";
#endif
    wxFileDialog dialog(this, "Choose testdrive2-enhanced", current.GetPath(), current.GetFullName(), filter,
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) program_->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

void LauncherDialog::View() {
    Save();
    const int i = scenery_->GetSelection(), k = stage_->GetSelection();
    if (i < 0 || k < 0) return;
    ViewerOptions options;
    options.program = wxFileName(program_->GetValue()).GetFullPath();
    options.gameDir = wxFileName(folder_->GetValue()).GetFullPath();
    options.stage = StageCode(sceneries_[i], sceneries_[i].stages[k]);
    options.startUnit = start_->GetValue();
    options.scale = scale_->GetSelection() + MIN_SCALE;
    options.resScale = resScale_->GetSelection() + MIN_RES;
    options.drawDistance = distance_->GetValue();
    options.showPosition = position_->GetValue();
    wxString error;
    if (!LaunchViewer(options, error)) wxMessageBox(error, APP_TITLE, wxOK | wxICON_ERROR, this);
}

void LauncherDialog::Save() {
    // A folder or program left at its default is stored empty, so it follows the launcher if it moves.
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    settings::SetString("Viewer", "GameFolder",
                        wxFileName(dir).SameAs(wxFileName(DefaultGameDir())) ? wxString() : dir);
    settings::SetString("Viewer", "Program",
                        wxFileName(program).SameAs(wxFileName(DefaultProgram())) ? wxString() : program);
    settings::SetString("Viewer", "Scenery", sceneryCode_);
    settings::SetInt("Viewer", "Stage", stageNumber_);
    settings::SetInt("Viewer", "StartUnit", start_->GetValue());
    settings::SetInt("Viewer", "Scale", scale_->GetSelection() + MIN_SCALE);
    settings::SetInt("Viewer", "ResScale", resScale_->GetSelection() + MIN_RES);
    settings::SetInt("Viewer", "DrawDistance", distance_->GetValue());
    settings::SetInt("Viewer", "ShowPosition", position_->GetValue() ? 1 : 0);
    settings::SaveWindowPosition("Viewer", this);
}

void LauncherDialog::About() {
    const wxString title = wxString("About ") + APP_TITLE;
    const wxString heading = wxString(APP_TITLE) + " " + APP_VERSION_TEXT;
    const wxString blurb = "Flies along the stages of Test Drive II: The Duel with the road view of "
                           "Test Drive II Enhanced.";
#ifdef __WXMSW__
    // The Windows task dialog.
    const wxString content = wxString::Format(
        "%s\n\n"
        "Author: Krzysztof Kania\n"
        "Website: <a href=\"%s\">kkania.com</a>\n"
        "Support: <a href=\"%s\">buymeacoffee.com/krzysztofkania</a>",
        blurb, WEBSITE, SUPPORT);
    wxIcon icon;
    icon.CopyFromBitmap(AppBitmap(FromDIP(32)));
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = static_cast<HWND>(GetHWND());
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = title.wc_str();
    dialog.hMainIcon = static_cast<HICON>(icon.GetHICON());
    dialog.pszMainInstruction = heading.wc_str();
    dialog.pszContent = content.wc_str();
    dialog.pfCallback = AboutCallback;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
#else
    wxDialog dialog(this, wxID_ANY, title);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(new wxStaticBitmap(&dialog, wxID_ANY, AppBitmap(dialog.FromDIP(48))), 0, wxALL, dialog.FromDIP(12));

    auto* text = new wxBoxSizer(wxVERTICAL);
    auto* headingText = new wxStaticText(&dialog, wxID_ANY, heading);
    headingText->SetFont(dialog.GetFont().Bold().Scaled(1.3f));
    text->Add(headingText, 0, wxBOTTOM, dialog.FromDIP(8));
    text->Add(new wxStaticText(&dialog, wxID_ANY, blurb), 0, wxBOTTOM, dialog.FromDIP(12));
    text->Add(new wxStaticText(&dialog, wxID_ANY, "Author: Krzysztof Kania"));
    AddLink(&dialog, text, "Website:", "kkania.com", WEBSITE);
    AddLink(&dialog, text, "Support:", "buymeacoffee.com/krzysztofkania", SUPPORT);
    body->Add(text, 1, wxTOP | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(body, 1, wxEXPAND);
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, dialog.FromDIP(8));
    dialog.SetSizerAndFit(all);
    dialog.CentreOnParent();
    dialog.ShowModal();
#endif
}
