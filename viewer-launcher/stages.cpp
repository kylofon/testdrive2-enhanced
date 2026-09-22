// stages.cpp -- SCENES.DAT, the stage files and starting the viewer.
#include "stages.h"

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

}  // namespace

bool FilePresent(const wxString& dir, const wxString& name) { return !dir.empty() && !FindFile(dir, name).empty(); }

// SCENES.DAT is read by the game with fscanf("%s %s %d") per scenery: code, name
// (underscores for spaces) and number of stages; a Ctrl-Z ends the text.
std::vector<Scenery> ReadSceneries(const wxString& gameDir) {
    std::vector<Scenery> list;
    if (gameDir.empty()) return list;
    const wxString path = FindFile(gameDir, "SCENES.DAT");
    wxString text;
    wxFFile file;
    {
        wxLogNull quiet;
        if (path.empty() || !file.Open(path, "rb") || !file.ReadAll(&text, wxConvISO8859_1)) return list;
    }
    text = text.BeforeFirst('\x1A');
    wxArrayString words = wxStringTokenize(text, " \t\r\n", wxTOKEN_STRTOK);
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

bool LaunchViewer(const ViewerOptions& o, wxString& error) {
    if (!wxFileName::FileExists(o.program)) {
        error = wxString::Format("The game's program isn't there:\n\n%s", o.program);
        return false;
    }
    std::vector<wxString> args{o.program,
                               "--game-dir", o.gameDir,
                               "--scale", wxString::Format("%d", o.scale),
                               "--res-scale", wxString::Format("%d", o.resScale),
                               "--draw-distance", wxString::Format("%d", o.drawDistance),
                               "--show-position", o.showPosition ? "on" : "off",
                               "--viewer", o.stage,
                               "--viewer-start", wxString::Format("%d", o.startUnit)};

    std::vector<std::wstring> wide;
    for (const wxString& a : args) wide.push_back(a.ToStdWstring());
    std::vector<const wchar_t*> argv;
    for (const std::wstring& w : wide) argv.push_back(w.c_str());
    argv.push_back(nullptr);

    wxExecuteEnv env;  // an empty variable map: the viewer inherits the launcher's environment
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
