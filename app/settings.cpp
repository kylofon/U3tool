// settings.cpp -- settings.ini, read and written with wxFileConfig.
#include "settings.h"

#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/toplevel.h>
#include <wx/utils.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include <cstdio>

namespace settings {
namespace {

// The Win32 build wrote paths with bare backslashes, so nothing is escaped.
wxFileConfig& Config() {
    static wxFileConfig* config = new wxFileConfig(
        wxEmptyString, wxEmptyString, Folder() + wxFILE_SEP_PATH + "settings.ini", wxEmptyString,
        wxCONFIG_USE_LOCAL_FILE | wxCONFIG_USE_NO_ESCAPE_CHARACTERS);
    return *config;
}

wxString Path(const wxString& section, const wxString& key) { return "/" + section + "/" + key; }

struct Placement {
    long left = 0, top = 0, right = 0, bottom = 0;
    int maximized = 0;
};

// "left,top,right,bottom,maximised", as the Win32 build stores it.
bool Parse(const wxString& text, Placement& p) {
    return std::sscanf(text.utf8_str(), "%ld,%ld,%ld,%ld,%d", &p.left, &p.top, &p.right, &p.bottom,
                       &p.maximized) == 5;
}

wxString Format(const Placement& p) {
    return wxString::Format("%ld,%ld,%ld,%ld,%d", p.left, p.top, p.right, p.bottom, p.maximized);
}

}  // namespace

wxString Folder() {
#ifdef __WXMSW__
    wxString dir = wxStandardPaths::Get().GetUserConfigDir() + "\\Ultima III Assistant";
#else
    wxString base;
    if (!wxGetEnv("XDG_CONFIG_HOME", &base) || base.empty()) base = wxGetHomeDir() + "/.config";
    wxString dir = base + "/ultima3-assistant";
#endif
    if (!wxFileName::DirExists(dir)) wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

void SaveWindow(const wxString& key, wxTopLevelWindow* window) {
    Placement p;
#ifdef __WXMSW__
    // The normal (restored) rectangle, even while maximised or minimised.
    WINDOWPLACEMENT native{};
    native.length = sizeof native;
    if (!GetWindowPlacement(static_cast<HWND>(window->GetHWND()), &native)) return;
    const RECT& r = native.rcNormalPosition;
    p = {r.left, r.top, r.right, r.bottom, native.showCmd == SW_SHOWMAXIMIZED ? 1 : 0};
#else
    if (window->IsMaximized() || window->IsIconized()) {
        // The restored size isn't known here: keep the one saved before.
        if (!Parse(GetString("Windows", key, ""), p)) return;
        p.maximized = window->IsMaximized() ? 1 : 0;
    } else {
        const wxRect r = window->GetRect();
        p = {r.GetLeft(), r.GetTop(), r.GetRight() + 1, r.GetBottom() + 1, 0};
    }
#endif
    SetString("Windows", key, Format(p));
}

bool RestoreWindow(const wxString& key, wxTopLevelWindow* window) {
    Placement p;
    if (!Parse(GetString("Windows", key, ""), p)) return false;
    // Skip nonsense sizes, and places no monitor covers any more.
    const wxRect rect(wxPoint(p.left, p.top), wxPoint(p.right - 1, p.bottom - 1));
    if (rect.width < 100 || rect.height < 60) return false;
    if (wxDisplay::GetFromPoint(rect.GetTopLeft() + wxPoint(rect.width / 2, 0)) == wxNOT_FOUND &&
        wxDisplay::GetFromPoint(rect.GetTopLeft()) == wxNOT_FOUND)
        return false;

#ifdef __WXMSW__
    // Set the normal rectangle natively, without showing, so it's the same
    // workspace-relative rectangle GetWindowPlacement gave.
    WINDOWPLACEMENT native{};
    native.length = sizeof native;
    const HWND hwnd = static_cast<HWND>(window->GetHWND());
    GetWindowPlacement(hwnd, &native);
    native.flags = 0;
    native.rcNormalPosition = {p.left, p.top, p.right, p.bottom};
    native.showCmd = SW_HIDE;
    if (!SetWindowPlacement(hwnd, &native)) return false;
#else
    window->SetSize(rect);
#endif
    window->Show();
    if (p.maximized) window->Maximize();
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
