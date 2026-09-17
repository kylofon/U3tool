// maps.h -- World and Dungeons map windows, drawn from the game's own files.
#pragma once

#include <windows.h>

#include <string>

#include "reader.h"

namespace u3maps {

enum Kind { WORLD, DUNGEONS, KIND_COUNT };

using Location = u3::Location;

// Registers the window classes; call once at startup.
void Register(HINSTANCE inst, HICON icon, HICON smallIcon);

// Opens a map window cascaded from `owner`, or brings it forward if it's open.
void Show(Kind kind, HWND owner, HFONT font, int dpi, bool topmost);

// Applies the Always on top preference to any open map windows.
void SetTopmost(bool topmost);

// Called with each update: moves the cursor, follows the party onto new maps and
// records explored dungeon cells. `gameFolder` may be empty when unknown.
void UpdateLocation(const Location& where, const std::wstring& gameFolder);

// At startup: reopens the map windows that were open when the app last closed.
void RestoreOpenWindows(HWND owner, HFONT font, int dpi, bool topmost);

// As the app closes: remembers which map windows are open, and where.
void SaveOpenWindows();

}  // namespace u3maps
