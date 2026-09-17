// mapframe.h -- the World and Dungeons map windows, drawn from the game's own
// files with the tables and decoding in core/mapdata.
#pragma once

#include <wx/string.h>

#include "reader.h"

class wxWindow;

namespace mapwin {

enum Kind { WORLD, DUNGEONS, KIND_COUNT };

// Opens a map window cascaded from `owner`, or brings it forward if it's open.
void Show(Kind kind, wxWindow* owner, bool topmost);

// Applies the Always on top preference to the open windows.
void SetTopmost(bool topmost);

// Called with each update: moves the cursor, follows the party onto new maps
// and records explored dungeon cells. `gameFolder` may be empty when unknown.
void UpdateLocation(const u3::Location& where, const wxString& gameFolder);

// At startup: reopens the windows that were open when the app last closed.
void RestoreOpenWindows(wxWindow* owner, bool topmost);

// As the app closes: remembers which windows are open and where, then closes them.
void SaveAndCloseAll();

}  // namespace mapwin
