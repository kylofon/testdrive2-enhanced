// settings.cpp -- settings.ini, read and written with wxFileConfig.
#include "settings.h"

#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/toplevel.h>
#include <wx/utils.h>

#include <cstdio>

namespace settings {
namespace {

// Paths are written with bare backslashes, so nothing is escaped.
wxFileConfig& Config() {
    static wxFileConfig* config = new wxFileConfig(
        wxEmptyString, wxEmptyString, Folder() + wxFILE_SEP_PATH + "settings.ini", wxEmptyString,
        wxCONFIG_USE_LOCAL_FILE | wxCONFIG_USE_NO_ESCAPE_CHARACTERS);
    return *config;
}

wxString Path(const wxString& section, const wxString& key) { return "/" + section + "/" + key; }

}  // namespace

wxString Folder() {
#ifdef __WXMSW__
    wxString dir = wxStandardPaths::Get().GetUserConfigDir() + "\\TD2 Enhanced";
#else
    wxString base;
    if (!wxGetEnv("XDG_CONFIG_HOME", &base) || base.empty()) base = wxGetHomeDir() + "/.config";
    wxString dir = base + "/td2-enhanced";
#endif
    if (!wxFileName::DirExists(dir)) wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

// "left,top"
void SaveWindowPosition(const wxString& key, wxTopLevelWindow* window) {
    if (window->IsIconized()) return;
    const wxPoint at = window->GetPosition();
    SetString("Windows", key, wxString::Format("%d,%d", at.x, at.y));
}

bool RestoreWindowPosition(const wxString& key, wxTopLevelWindow* window) {
    int x = 0, y = 0;
    if (std::sscanf(GetString("Windows", key, "").utf8_str(), "%d,%d", &x, &y) != 2) return false;
    // The title bar must still be on a monitor.
    const wxSize size = window->GetSize();
    if (wxDisplay::GetFromPoint(wxPoint(x + size.x / 2, y + 8)) == wxNOT_FOUND &&
        wxDisplay::GetFromPoint(wxPoint(x + 8, y + 8)) == wxNOT_FOUND)
        return false;
    window->Move(x, y);
    return true;
}

int GetInt(const wxString& section, const wxString& key, int fallback) {
    return static_cast<int>(Config().ReadLong(Path(section, key), fallback));
}

void SetInt(const wxString& section, const wxString& key, int value) {
    Config().Write(Path(section, key), static_cast<long>(value));
    Config().Flush();
}

wxString GetString(const wxString& section, const wxString& key, const wxString& fallback) {
    return Config().Read(Path(section, key), fallback);
}

void SetString(const wxString& section, const wxString& key, const wxString& value) {
    Config().Write(Path(section, key), value);
    Config().Flush();
}

}  // namespace settings
