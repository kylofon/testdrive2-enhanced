// settings.h -- settings kept between runs in settings.ini: under
// %APPDATA%\TD2 Enhanced on Windows, and ~/.config/td2-enhanced on Linux.
// Every change is written straight away.
#pragma once

#include <wx/string.h>

class wxTopLevelWindow;

namespace settings {

// The folder holding settings.ini, created if needed.
wxString Folder();

// Remembers where a window is.
void SaveWindowPosition(const wxString& key, wxTopLevelWindow* window);

// Moves a window to where it was. Returns false, leaving it alone, if nothing
// usable was saved or no monitor covers that place any more.
bool RestoreWindowPosition(const wxString& key, wxTopLevelWindow* window);

int GetInt(const wxString& section, const wxString& key, int fallback);
void SetInt(const wxString& section, const wxString& key, int value);

wxString GetString(const wxString& section, const wxString& key, const wxString& fallback);
void SetString(const wxString& section, const wxString& key, const wxString& value);

}  // namespace settings
