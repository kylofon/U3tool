// refdata.h -- the Weapons, Armour and Spells reference tables, and who in
// the party can use each row.
//
// The tables come from the Gamer Corner Ultima III guide
// (https://guides.gamercorner.net/ultimaiii/). Their class columns agree with
// the limits EXODUS.BIN enforces, which reader.cpp uses for equipping.
#pragma once

#include <string>
#include <vector>

#include "reader.h"

namespace u3::ref {

enum Kind { WEAPONS, ARMOUR, SPELLS, KIND_COUNT };

enum class Align { Left, Right };

struct Column {
    const wchar_t* title;
    int width;  // at 96 dpi
    Align align;
};

struct Group {
    const wchar_t* title;  // nullptr for a plain, ungrouped list
    const wchar_t* const* cells;
    int cellCount;
};

struct Table {
    const wchar_t* title;
    const Column* columns;
    int columnCount;
    const Group* groups;
    int groupCount;
    const wchar_t* note;
    int nameColumn, classesColumn;
    const wchar_t* verb;  // what a character does with a row: ready, wear or cast

    bool Grouped() const { return groups[0].title != nullptr; }
    int RowCount() const;
    // The cells of a row, counting rows across groups; nullptr if out of range.
    const wchar_t* const* Row(int row) const;
};

extern const Table TABLES[KIND_COUNT];

enum SpellColumn { SPELL_KEY, SPELL_NAME, SPELL_MP, SPELL_EFFECT, SPELL_WHERE, SPELL_CURES, SPELL_CLASSES };

// For the Classes column header.
extern const wchar_t* const CLASS_LEGEND;

struct Caster {
    std::wstring name;
    wchar_t classCode = 0;  // F, C, W, T, P, B, L, I, D, A or R
    bool alive = false;     // Good or Poisoned
    int mp = 0;
};

// What the Spells "Castable only" filter and the row tooltips go by.
struct PartyState {
    bool live = false;
    int map = 0;          // 0x00 overworld, 0x01 dungeon, 0x80 combat, others for towns and castles
    int combatTurn = -1;  // in combat, whose turn it is
    int count = 0;
    Caster members[4];
};

// `party` may be null when none is connected.
PartyState PartyStateOf(const Party* party, int combatTurn);

// Which spells to show, one flag per row: in combat, those the member whose
// turn it is can cast; elsewhere, those anyone in the party can. `whom` gets a caption.
std::vector<bool> CastableRows(const PartyState& party, std::wstring* whom);

// Which party members can ready, wear or cast a row's item or spell; empty
// for no row.
std::wstring UsersText(Kind kind, int row, const PartyState& party);

}  // namespace u3::ref
