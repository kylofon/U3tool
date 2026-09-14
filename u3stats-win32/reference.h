// reference.h -- Weapons, Armour and Spells reference windows.
#pragma once

#include <windows.h>

#include <string>

namespace u3ref {

enum Kind { WEAPONS, ARMOUR, SPELLS, KIND_COUNT };

struct Caster {
    std::wstring name;
    wchar_t classCode = 0;  // F, C, W, T, P, B, L, I, D, A or R
    bool alive = false;     // Good or Poisoned
    int mp = 0;
};

// What the Spells window's "Castable only" filter goes by.
struct PartyState {
    bool live = false;
    int map = 0;          // 0x00 overworld, 0x01 dungeon, 0x80 combat, others for towns and castles
    int combatTurn = -1;  // in combat, whose turn it is
    int count = 0;
    Caster members[4];
};

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
