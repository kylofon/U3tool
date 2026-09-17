// referenceframe.h -- the Weapons, Armour and Spells reference windows,
// showing the tables in core/refdata.
#pragma once

#include "refdata.h"

class wxWindow;

namespace refwin {

using Kind = u3::ref::Kind;

// Opens a window cascaded from `owner`, or brings it forward if it's open.
void Show(Kind kind, wxWindow* owner, bool topmost);

// Applies the Always on top preference to the open windows.
void SetTopmost(bool topmost);

// Called with each party update; refilters the Spells window if needed.
void UpdateParty(const u3::ref::PartyState& party);

// At startup: reopens the windows that were open when the app last closed.
void RestoreOpenWindows(wxWindow* owner, bool topmost);

// As the app closes: remembers which windows are open and where, then closes them.
void SaveAndCloseAll();

}  // namespace refwin
