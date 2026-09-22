// app.cpp -- TD2 Map Viewer: picks a stage of Test Drive II and starts the
// map viewer of testdrive2-enhanced on it.
#include <wx/app.h>

#include "launcher.h"

class ViewerApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("TD2 Map Viewer");
        SetVendorName("Krzysztof Kania");
        if (!wxApp::OnInit()) return false;
        auto* dialog = new LauncherDialog;
        SetTopWindow(dialog);
        dialog->Show();
        return true;
    }
};

wxIMPLEMENT_APP(ViewerApp);
