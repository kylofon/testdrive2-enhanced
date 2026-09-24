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

const char* const APP_TITLE = "Test Drive II Enhanced";

namespace {

const char* const WEBSITE = "https://kkania.com";
const char* const SUPPORT = "https://buymeacoffee.com/krzysztofkania";
const char* const SECTION = "Game";

const int MIN_SCALE = 1, MAX_SCALE = 6, DEFAULT_SCALE = 3;
const int MIN_RES = 1, MAX_RES = 8, DEFAULT_RES = 4;
const int MIN_DISTANCE = 60, MAX_DISTANCE = 240, DEFAULT_DISTANCE = 180;
// The game's speed: timer ticks (100 a second) per simulation step (testdrive2-enhanced --sim-ticks); the original: 10.
const int MIN_SIM_TICKS = 3, MAX_SIM_TICKS = 20, DEFAULT_SIM_TICKS = 6;

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

// A label and a choice in a row of a two-column grid.
wxChoice* ChoiceRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxString& tip) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(220), -1));
    choice->SetToolTip(tip);
    grid->Add(choice, 0, wxALIGN_CENTER_VERTICAL);
    return choice;
}

// Fills a car list: "Default" first, then the cars; selects `code` (Default when it isn't there).
void FillCarChoice(wxChoice* choice, const std::vector<Car>& cars, wxString& code) {
    choice->Clear();
    choice->Append("Default (the game's last choice)");
    int pick = 0;
    for (size_t i = 0; i < cars.size(); ++i) {
        choice->Append(wxString::Format("%s (%s)", cars[i].name, cars[i].code.Upper()));
        if (cars[i].code.CmpNoCase(code) == 0) pick = static_cast<int>(i) + 1;
    }
    choice->SetSelection(pick);
    if (pick == 0) code.clear();
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
    program_ = PathRow(fb, filesGrid, "&Program:", "testdrive2-enhanced, the game.", &browseProgram);
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

    // Start
    auto* startBox = new wxStaticBoxSizer(wxVERTICAL, this, "Start");
    wxWindow* sb = startBox->GetStaticBox();
    quick_ = new wxCheckBox(sb, wxID_ANY, "Start a &race at once (no intro, menus or difficulty screen)");
    quick_->SetToolTip("Straight onto the road of the stage below. After the race the game goes on as usual.");
    startBox->Add(quick_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    auto* startGrid = new wxFlexGridSizer(2, gap, gap);
    scenery_ = ChoiceRow(sb, startGrid, "S&cenery:", "Default: the scenery last chosen in the game's menu.");
    stage_ = ChoiceRow(sb, startGrid, "&Stage:", "The stage the race starts on; the game goes on to the next ones.");
    race_ = ChoiceRow(sb, startGrid, "Race &against:", "The game's two races: against the clock or the opponent.");
    race_->Append("The clock");
    race_->Append("The opponent");
    car_ = ChoiceRow(sb, startGrid, "&Car:",
                     "Your car, as if chosen in the game's menu (also when the game starts with its menus).");
    opponent_ = ChoiceRow(sb, startGrid, "&Opponent:", "The opponent's car, as if chosen in the game's menu.");
    startBox->Add(startGrid, 0, wxALL, gap);
    quick_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateState(); });
    scenery_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = scenery_->GetSelection();
        sceneryCode_ = i > 0 ? sceneries_[i - 1].code : wxString();
        FillStages();
    });
    stage_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = scenery_->GetSelection(), k = stage_->GetSelection();
        if (i > 0 && k >= 0) stageNumber_ = sceneries_[i - 1].stages[k];
    });
    race_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateState(); });
    car_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = car_->GetSelection();
        carCode_ = i > 0 ? cars_[i - 1].code : wxString();
    });
    opponent_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = opponent_->GetSelection();
        opponentCode_ = i > 0 ? opponents_[i - 1].code : wxString();
    });

    // Options
    auto* optionsBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
    wxWindow* ob = optionsBox->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, gap, gap);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Window size:"), 0, wxALIGN_CENTER_VERTICAL);
    scale_ = new wxChoice(ob, wxID_ANY);
    for (int s = MIN_SCALE; s <= MAX_SCALE; ++s) scale_->Append(wxString::Format(L"%d × %d", 320 * s, 240 * s));
    scale_->SetToolTip("The window's size when the game starts. Alt+Enter switches to full screen.");
    grid->Add(scale_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(ob, wxID_ANY, "R&esolution:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* resRow = new wxBoxSizer(wxHORIZONTAL);
    resScale_ = new wxChoice(ob, wxID_ANY);
    for (int s = MIN_RES; s <= MAX_RES; ++s) resScale_->Append(wxString::Format(L"%d × %d", 320 * s, 200 * s));
    resScale_->SetToolTip("The resolution the road is drawn at. Lower it on a slower computer.");
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
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Game speed:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* speedRow = new wxBoxSizer(wxHORIZONTAL);
    speed_ = new wxSpinCtrl(ob, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1),
                            wxSP_ARROW_KEYS, MIN_SIM_TICKS, MAX_SIM_TICKS, DEFAULT_SIM_TICKS);
    speed_->SetToolTip("Timer ticks (100 a second) per simulation step. The game moves every car once per step, so "
                       "fewer ticks make everything faster: your car, the opponent, the traffic and the police. "
                       "The race clock always counts real seconds. The original steps every 10 ticks; with the "
                       "smooth picture the road then seems to pass slowly, 6 is about as fast as it felt on the "
                       "jerky original.");
    speedRow->Add(speed_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    speedRow->Add(GreyText(ob, "ticks a step: 6 recommended, 10 as the original (fewer = faster)"), 0,
                  wxALIGN_CENTER_VERTICAL);
    grid->Add(speedRow, 0, wxALIGN_CENTER_VERTICAL);
    optionsBox->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    auto* checks = new wxBoxSizer(wxVERTICAL);
    auto addCheck = [&](const wxString& label, const wxString& tip) {
        auto* check = new wxCheckBox(ob, wxID_ANY, label);
        check->SetToolTip(tip);
        checks->Add(check, 0, wxTOP, small);
        return check;
    };
    road_ = addCheck("Enhanced r&oad: lighter and darker bands across the road",
                     "The road and its shoulders alternate between a lighter and a darker shade every two road "
                     "units, as in Test Drive Enhanced, so the speed shows. Off: the original's plain road.");
    sides_ = addCheck("Enhanced side scener&y: the bands carry on beside the road",
                      "The ground beside the road (grass, sand, earth) alternates between a lighter and a darker "
                      "shade with the road's bands, as in Out Run. Rock faces and mountains stay plain.");
    mix_ = addCheck("M&ix cars between sceneries",
                    "Some of the traffic is drawn as the other sceneries' cars: the Beetle and the grey Saab in "
                    "California and the Master Scenery, the Mercedes in Europe. At least one of each of the "
                    "scenery's own cars stays on every stage.");
    valley_ = addCheck("&Valley floor far below drop-offs",
                       "Off: the sky continues below the cliffs beside the road, as in the original.");
    detail_ = addCheck("&Most detailed sprites at every distance",
                       "Cars and roadside objects use their largest sprite, scaled to their true size. Off: the "
                       "sprite size is chosen by distance, as in the original.");
    position_ = addCheck("Show the &position (F9 switches it on and off)",
                         "The stage, road unit and lateral position in the top left corner of the road view, e.g. "
                         "\"CCC0 790 X160\", to quote when reporting a place.");
    classic_ = addCheck("C&lassic: the original road view at 15 frames a second",
                        "The original renderer, for comparison. The options above do not apply.");
    optionsBox->Add(checks, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    classic_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateState(); });

    // Controls
    auto* keysBox = new wxStaticBoxSizer(wxVERTICAL, this, "Keys in the game");
    wxWindow* kb = keysBox->GetStaticBox();
    auto* keys = new wxFlexGridSizer(2, small, FromDIP(16));
    const char* const KEYS[][2] = {
        {"Arrows / keypad", "steer, accelerate and brake; shift as in the original"},
        {"Esc", "leave the drive or the menu"},
        {"Ctrl+P", "pause"},
        {"Ctrl+S / Ctrl+Q", "sound / music on and off"},
        {"Ctrl+J", "joystick (a gamepad) on and calibrate"},
        {"F9", "position on / off"},
        {"Alt+Enter", "full screen"},
    };
    for (const auto& k : KEYS) {
        keys->Add(new wxStaticText(kb, wxID_ANY, k[0]));
        keys->Add(GreyText(kb, k[1]));
    }
    keysBox->Add(keys, 0, wxALL, gap);

    // Buttons
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* about = new wxButton(this, wxID_ABOUT, "&About");
    play_ = new wxButton(this, wxID_ANY, "&Play");
    auto* close = new wxButton(this, wxID_CLOSE, "Close");
    buttons->Add(about);
    buttons->AddStretchSpacer();
    buttons->Add(play_, 0, wxRIGHT, gap);
    buttons->Add(close);
    play_->SetDefault();
    SetEscapeId(wxID_CLOSE);
    about->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { About(); });
    play_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Play(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    // Two columns: the files and the start on the left, the options and the keys on the right.
    auto* left = new wxBoxSizer(wxVERTICAL);
    left->Add(filesBox, 0, wxEXPAND);
    left->Add(startBox, 1, wxEXPAND | wxTOP, margin);
    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(optionsBox, 0, wxEXPAND);
    right->Add(keysBox, 1, wxEXPAND | wxTOP, margin);
    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    columns->Add(left, 0, wxEXPAND);
    columns->Add(right, 0, wxEXPAND | wxLEFT, margin);
    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(columns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    SetSizer(all);

    // Settings from the last run.
    wxString dir = settings::GetString(SECTION, "GameFolder", "");
    wxString program = settings::GetString(SECTION, "Program", "");
    folder_->ChangeValue(dir.empty() ? DefaultGameDir() : dir);
    program_->ChangeValue(program.empty() ? DefaultProgram() : program);
    quick_->SetValue(settings::GetInt(SECTION, "QuickStart", 0) != 0);
    sceneryCode_ = settings::GetString(SECTION, "Scenery", "");
    stageNumber_ = settings::GetInt(SECTION, "Stage", 0);
    race_->SetSelection(settings::GetInt(SECTION, "AgainstOpponent", 0) != 0 ? 1 : 0);
    carCode_ = settings::GetString(SECTION, "Car", "");
    opponentCode_ = settings::GetString(SECTION, "Opponent", "");
    scale_->SetSelection(
        wxMax(MIN_SCALE, wxMin(MAX_SCALE, settings::GetInt(SECTION, "Scale", DEFAULT_SCALE))) - MIN_SCALE);
    resScale_->SetSelection(wxMax(MIN_RES, wxMin(MAX_RES, settings::GetInt(SECTION, "ResScale", DEFAULT_RES))) - MIN_RES);
    distance_->SetValue(settings::GetInt(SECTION, "DrawDistance", DEFAULT_DISTANCE));
    speed_->SetValue(wxMax(MIN_SIM_TICKS, wxMin(MAX_SIM_TICKS, settings::GetInt(SECTION, "GameSpeed", DEFAULT_SIM_TICKS))));
    road_->SetValue(settings::GetInt(SECTION, "EnhancedRoad", 1) != 0);
    sides_->SetValue(settings::GetInt(SECTION, "EnhancedSides", 1) != 0);
    mix_->SetValue(settings::GetInt(SECTION, "MixCars", 0) != 0);
    valley_->SetValue(settings::GetInt(SECTION, "Valley", 0) != 0);
    detail_->SetValue(settings::GetInt(SECTION, "DetailMax", 1) != 0);
    position_->SetValue(settings::GetInt(SECTION, "ShowPosition", 0) != 0);
    classic_->SetValue(settings::GetInt(SECTION, "Classic", 0) != 0);
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
    if (!settings::RestoreWindowPosition(SECTION, this)) Centre();
}

void LauncherDialog::Reload() {
    const wxString dir = folder_->GetValue();
    sceneries_ = ReadSceneries(dir);
    scenery_->Clear();
    scenery_->Append("Default (the game's last choice)");
    int pick = 0;
    for (size_t i = 0; i < sceneries_.size(); ++i) {
        scenery_->Append(wxString::Format("%s (%s)", sceneries_[i].name, sceneries_[i].code.Upper()));
        if (sceneries_[i].code.CmpNoCase(sceneryCode_) == 0) pick = static_cast<int>(i) + 1;
    }
    scenery_->SetSelection(pick);
    sceneryCode_ = pick > 0 ? sceneries_[pick - 1].code : wxString();
    FillStages();
    FillCars();
    UpdateState();
}

void LauncherDialog::FillStages() {
    stage_->Clear();
    const int i = scenery_->GetSelection();
    if (i > 0) {
        const Scenery& s = sceneries_[i - 1];
        int pick = 0;
        for (size_t k = 0; k < s.stages.size(); ++k) {
            stage_->Append(wxString::Format("Stage %d (%s)", s.stages[k] + 1, StageCode(s, s.stages[k])));
            if (s.stages[k] == stageNumber_) pick = static_cast<int>(k);
        }
        stage_->SetSelection(pick);
        stageNumber_ = s.stages[pick];
    } else {
        stage_->Append("Stage 1");  // the game's own scenery starts at its first stage
        stage_->SetSelection(0);
    }
    UpdateState();
}

void LauncherDialog::FillCars() {
    const wxString dir = folder_->GetValue();
    cars_ = ReadCars(dir);
    opponents_.clear();
    for (const Car& c : cars_)
        if (c.opponent) opponents_.push_back(c);
    FillCarChoice(car_, cars_, carCode_);
    FillCarChoice(opponent_, opponents_, opponentCode_);
}

void LauncherDialog::UpdateState() {
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    const bool haveProgram = wxFileName::FileExists(program);
    wxString note;
    if (!FilePresent(dir, "SCENES.DAT") || !FilePresent(dir, "CARS.DAT"))
        note = "This folder needs the game's files: CARS.DAT, SCENES.DAT, TD2EGA.EXE and the rest.";
    else if (sceneries_.empty())
        note = "SCENES.DAT lists no scenery whose stage files are in this folder.";
    else if (cars_.empty())
        note = "CARS.DAT lists no car whose files are in this folder.";
    else if (!FilePresent(dir, "TD2EGA.EXE"))
        note = "This folder needs TD2EGA.EXE.";
    else if (!haveProgram)
        note = wxString::Format("%s isn't there.", wxFileName(program).GetFullName());
    else {
        int stages = 0;
        for (const Scenery& s : sceneries_) stages += static_cast<int>(s.stages.size());
        note = wxString::Format("Found %d %s with %d stages and %d %s.", static_cast<int>(sceneries_.size()),
                                sceneries_.size() == 1 ? "scenery" : "sceneries", stages,
                                static_cast<int>(cars_.size()), cars_.size() == 1 ? "car" : "cars");
    }
    const bool ok = haveProgram && !sceneries_.empty() && !cars_.empty() && FilePresent(dir, "TD2EGA.EXE");
    statusIcon_->Show(!ok);
    statusNote_->SetLabel(note);

    const bool quick = quick_->GetValue(), opponent = race_->GetSelection() == 1;
    scenery_->Enable(quick && !sceneries_.empty());
    stage_->Enable(quick && scenery_->GetSelection() > 0);
    race_->Enable(quick);
    car_->Enable(!cars_.empty());
    opponent_->Enable(!opponents_.empty() && (!quick || opponent));

    const bool enhanced = !classic_->GetValue();
    for (wxWindow* w : std::initializer_list<wxWindow*>{resScale_, distance_, road_, sides_, mix_, valley_, detail_, position_})
        w->Enable(enhanced);

    play_->Enable(ok);
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

void LauncherDialog::Play() {
    Save();
    GameOptions options;
    options.program = wxFileName(program_->GetValue()).GetFullPath();
    options.gameDir = wxFileName(folder_->GetValue()).GetFullPath();
    if (quick_->GetValue()) {
        const int i = scenery_->GetSelection(), k = stage_->GetSelection();
        options.startStage = i > 0 && k >= 0 ? StageCode(sceneries_[i - 1], sceneries_[i - 1].stages[k])
                                             : wxString("default");
        options.againstOpponent = race_->GetSelection() == 1;
    }
    options.car = carCode_;
    options.opponent = opponentCode_;
    options.scale = scale_->GetSelection() + MIN_SCALE;
    options.resScale = resScale_->GetSelection() + MIN_RES;
    options.drawDistance = distance_->GetValue();
    options.simTicks = speed_->GetValue();
    options.enhancedRoad = road_->GetValue();
    options.enhancedSides = sides_->GetValue();
    options.mixCars = mix_->GetValue();
    options.valley = valley_->GetValue();
    options.detailMax = detail_->GetValue();
    options.showPosition = position_->GetValue();
    options.classic = classic_->GetValue();
    wxString error;
    if (!LaunchGame(options, error)) wxMessageBox(error, APP_TITLE, wxOK | wxICON_ERROR, this);
}

void LauncherDialog::Save() {
    // A folder or program left at its default is stored empty, so it follows the launcher if it moves.
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    settings::SetString(SECTION, "GameFolder",
                        wxFileName(dir).SameAs(wxFileName(DefaultGameDir())) ? wxString() : dir);
    settings::SetString(SECTION, "Program",
                        wxFileName(program).SameAs(wxFileName(DefaultProgram())) ? wxString() : program);
    settings::SetInt(SECTION, "QuickStart", quick_->GetValue() ? 1 : 0);
    settings::SetString(SECTION, "Scenery", sceneryCode_);
    settings::SetInt(SECTION, "Stage", stageNumber_);
    settings::SetInt(SECTION, "AgainstOpponent", race_->GetSelection() == 1 ? 1 : 0);
    settings::SetString(SECTION, "Car", carCode_);
    settings::SetString(SECTION, "Opponent", opponentCode_);
    settings::SetInt(SECTION, "Scale", scale_->GetSelection() + MIN_SCALE);
    settings::SetInt(SECTION, "ResScale", resScale_->GetSelection() + MIN_RES);
    settings::SetInt(SECTION, "DrawDistance", distance_->GetValue());
    settings::SetInt(SECTION, "GameSpeed", speed_->GetValue());
    settings::SetInt(SECTION, "EnhancedRoad", road_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "EnhancedSides", sides_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "MixCars", mix_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "Valley", valley_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "DetailMax", detail_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "ShowPosition", position_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "Classic", classic_->GetValue() ? 1 : 0);
    settings::SaveWindowPosition(SECTION, this);
}

void LauncherDialog::About() {
    const wxString title = wxString("About ") + APP_TITLE;
    const wxString heading = wxString(APP_TITLE) + " " + APP_VERSION_TEXT;
    const wxString blurb = "Starts Test Drive II: The Duel with the enhanced road view of Test Drive II Enhanced.";
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
