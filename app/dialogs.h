// dialogs.h -- the About box, Preferences and the "move how many" prompt.
#pragma once

#include <wx/string.h>

class wxWindow;

extern const char* const APP_TITLE;

void ShowAbout(wxWindow* parent);

// Asks whether to keep windows on top. Returns true when the answer changed.
bool ShowPreferences(wxWindow* parent, bool& alwaysOnTop);

// Asks for 1..max, starting at max. Returns 0 if cancelled.
int AskCount(wxWindow* parent, const wxString& prompt, int max);
