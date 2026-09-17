// app.cpp -- Ultima III Assistant, the wxWidgets build: a live party viewer and
// editor for Ultima III, for Windows and Linux.
#include <wx/app.h>

#include "mainframe.h"

#ifdef __WXMSW__
#include "crash.h"
#endif

class AssistantApp : public wxApp {
public:
    bool OnInit() override {
#ifdef __WXMSW__
        crash::Install();
#endif
        SetAppName("Ultima III Assistant");
        SetVendorName("Krzysztof Kania");
        if (!wxApp::OnInit()) return false;
        auto* frame = new MainFrame;
        SetTopWindow(frame);
        frame->RestoreLayout();
        return true;
    }
};

wxIMPLEMENT_APP(AssistantApp);
