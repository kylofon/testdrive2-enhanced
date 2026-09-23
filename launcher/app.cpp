// app.cpp -- the Test Drive II Enhanced launcher: the game folder, how the
// game starts and its options, then testdrive2-enhanced.
#include <wx/app.h>

#include "launcher.h"

class LauncherApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("TD2 Enhanced");
        SetVendorName("Krzysztof Kania");
        if (!wxApp::OnInit()) return false;
        auto* dialog = new LauncherDialog;
        SetTopWindow(dialog);
        dialog->Show();
        return true;
    }
};

wxIMPLEMENT_APP(LauncherApp);
