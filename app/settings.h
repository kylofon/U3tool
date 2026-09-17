// settings.h -- settings kept between runs in settings.ini: under
// %APPDATA%\Ultima III Assistant on Windows (the same file the Win32 build
// uses), and ~/.config/ultima3-assistant on Linux. Every change is written
// straight away.
#pragma once

#include <wx/string.h>

class wxTopLevelWindow;

namespace settings {

// The folder holding settings.ini, created if needed.
wxString Folder();

// Remembers a window's normal position and size, and whether it's maximised.
void SaveWindow(const wxString& key, wxTopLevelWindow* window);

// Applies a remembered placement and shows the window. Returns false, leaving
// the window hidden, if nothing usable was saved or it would be off-screen.
bool RestoreWindow(const wxString& key, wxTopLevelWindow* window);

int GetInt(const wxString& section, const wxString& key, int fallback);
void SetInt(const wxString& section, const wxString& key, int value);

wxString GetString(const wxString& section, const wxString& key, const wxString& fallback);
void SetString(const wxString& section, const wxString& key, const wxString& value);

}  // namespace settings
