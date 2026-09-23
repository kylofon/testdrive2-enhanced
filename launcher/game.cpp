// game.cpp -- SCENES.DAT, CARS.DAT, the files they need and starting the game.
#include "game.h"

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include <string>

namespace {

// A file of `dir` whose name matches `name` as written, in upper or in lower case
// (case matters outside Windows); empty if there is none.
wxString FindFile(const wxString& dir, const wxString& name) {
    for (const wxString& n : {name, name.Upper(), name.Lower()}) {
        const wxString path = wxFileName(dir, n).GetFullPath();
        if (wxFileName::FileExists(path)) return path;
    }
    return wxString();
}

// The words of one of the game's catalogue files, read as the game does with
// fscanf("%s"): whitespace separated, a Ctrl-Z ends the text.
wxArrayString CatalogueWords(const wxString& gameDir, const wxString& name) {
    if (gameDir.empty()) return wxArrayString();
    const wxString path = FindFile(gameDir, name);
    wxString text;
    wxFFile file;
    {
        wxLogNull quiet;
        if (path.empty() || !file.Open(path, "rb") || !file.ReadAll(&text, wxConvISO8859_1)) return wxArrayString();
    }
    return wxStringTokenize(text.BeforeFirst('\x1A'), " \t\r\n", wxTOKEN_STRTOK);
}

}  // namespace

bool FilePresent(const wxString& dir, const wxString& name) { return !dir.empty() && !FindFile(dir, name).empty(); }

// SCENES.DAT: code, name (underscores for spaces) and number of stages per scenery.
std::vector<Scenery> ReadSceneries(const wxString& gameDir) {
    std::vector<Scenery> list;
    const wxArrayString words = CatalogueWords(gameDir, "SCENES.DAT");
    for (size_t i = 0; i + 2 < words.size(); i += 3) {
        Scenery s;
        s.code = words[i];
        s.name = words[i + 1];
        s.name.Replace("_", " ");
        long stages = 0;
        if (!words[i + 2].ToLong(&stages)) break;
        for (int k = 0; k < stages && k < 10; ++k)
            if (FilePresent(gameDir, wxString::Format("%s%d.DAT", s.code, k))) s.stages.push_back(k);
        if (!s.stages.empty()) list.push_back(s);
    }
    return list;
}

// CARS.DAT: code and name (underscores for spaces) per car. A car is driven from
// <code>.BIN and its dashboard, and raced against from <code>O.BIN.
std::vector<Car> ReadCars(const wxString& gameDir) {
    std::vector<Car> list;
    const wxArrayString words = CatalogueWords(gameDir, "CARS.DAT");
    for (size_t i = 0; i + 1 < words.size(); i += 2) {
        Car c;
        c.code = words[i];
        c.name = words[i + 1];
        c.name.Replace("_", " ");
        if (!FilePresent(gameDir, c.code + ".BIN")) continue;
        c.opponent = FilePresent(gameDir, c.code + "O.BIN");
        list.push_back(c);
    }
    return list;
}

wxString StageCode(const Scenery& scenery, int stage) { return wxString::Format("%s%d", scenery.code.Upper(), stage); }

wxString LauncherDir() { return wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath(); }

wxString DefaultGameDir() { return wxFileName(LauncherDir(), "Game").GetFullPath(); }

wxString DefaultProgram() {
    wxFileName name(LauncherDir(), "testdrive2-enhanced");
#ifdef __WXMSW__
    name.SetExt("exe");
#endif
    return name.GetFullPath();
}

bool LaunchGame(const GameOptions& o, wxString& error) {
    if (!wxFileName::FileExists(o.program)) {
        error = wxString::Format("The game's program isn't there:\n\n%s", o.program);
        return false;
    }
    auto onOff = [](bool v) { return wxString(v ? "on" : "off"); };
    std::vector<wxString> args{o.program,
                               "--game-dir", o.gameDir,
                               "--scale", wxString::Format("%d", o.scale),
                               "--show-position", onOff(o.showPosition)};
    if (o.classic) {
        args.push_back("--classic");
    } else {
        for (const wxString& a : {wxString("--res-scale"), wxString::Format("%d", o.resScale),
                                  wxString("--draw-distance"), wxString::Format("%d", o.drawDistance),
                                  wxString("--enhanced-road"), onOff(o.enhancedRoad),
                                  wxString("--enhanced-sides"), onOff(o.enhancedSides),
                                  wxString("--mix-cars"), onOff(o.mixCars),
                                  wxString("--valley"), onOff(o.valley),
                                  wxString("--sprite-detail"), wxString(o.detailMax ? "max" : "auto")})
            args.push_back(a);
    }
    if (!o.car.empty()) args.insert(args.end(), {"--car", o.car});
    if (!o.opponent.empty()) args.insert(args.end(), {"--opponent", o.opponent});
    if (!o.startStage.empty())
        args.insert(args.end(), {"--start", o.startStage, "--race", o.againstOpponent ? "opponent" : "clock"});

    std::vector<std::wstring> wide;
    for (const wxString& a : args) wide.push_back(a.ToStdWstring());
    std::vector<const wchar_t*> argv;
    for (const std::wstring& w : wide) argv.push_back(w.c_str());
    argv.push_back(nullptr);

    wxExecuteEnv env;  // an empty variable map: the game inherits the launcher's environment
    env.cwd = wxFileName(o.program).GetPath();
    long pid;
    {
        wxLogNull quiet;  // wxExecute would show its own error box
        pid = wxExecute(argv.data(), wxEXEC_ASYNC, nullptr, &env);
    }
    if (pid == 0) {
        error = wxString::Format("Couldn't start %s.\n\n%s", wxFileName(o.program).GetFullName(),
                                 wxSysErrorMsgStr(wxSysErrorCode()));
        return false;
    }
    return true;
}
