// reference.h -- Weapons, Armour and Spells reference windows.
#pragma once

#include <windows.h>

#include <string>

#include "refdata.h"

namespace u3ref {

using u3::ref::ARMOUR;
using u3::ref::Kind;
using u3::ref::KIND_COUNT;
using u3::ref::PartyState;
using u3::ref::SPELLS;
using u3::ref::WEAPONS;

// Registers the reference window class; call once at startup.
void Register(HINSTANCE inst, HICON icon, HICON smallIcon);

// Opens a reference window cascaded from `owner`, or brings it forward if it's
// already open.
void Show(Kind kind, HWND owner, HFONT font, int dpi, bool topmost);

// Applies the Always on top preference to any open reference windows.
void SetTopmost(bool topmost);

// Called with each party update; refilters the Spells window if needed.
void UpdateParty(const PartyState& party);

// At startup: reopens the reference windows that were open when the app last closed.
void RestoreOpenWindows(HWND owner, HFONT font, int dpi, bool topmost);

// As the app closes: remembers which reference windows are open, and where.
void SaveOpenWindows();

}  // namespace u3ref
