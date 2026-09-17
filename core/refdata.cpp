// refdata.cpp -- reference table contents and party checks against them.
#include "refdata.h"

#include <cwchar>

namespace u3::ref {
namespace {

template <typename T, size_t N>
constexpr int CountOf(const T (&)[N]) { return static_cast<int>(N); }

// Weapons ------------------------------------------------------------------

const Column WEAPON_COLUMNS[] = {
    {L"Key", 40, Align::Left},    {L"Weapon", 80, Align::Left},    {L"Kind", 68, Align::Left},
    {L"Range", 62, Align::Left},  {L"Price", 70, Align::Right},    {L"Classes", 150, Align::Left},
    {L"Notes", 130, Align::Left},
};

const wchar_t* const WEAPON_CELLS[] = {
    L"A", L"Hands",   L"Mundane", L"Melee",  L"–",           L"All",                        L"",
    L"B", L"Dagger",  L"Mundane", L"Ranged", L"5 gold",      L"All",                        L"Lost if thrown",
    L"C", L"Mace",    L"Mundane", L"Melee",  L"30 gold",     L"F, C, T, P, B, L, I, D, R",  L"",
    L"D", L"Sling",   L"Mundane", L"Ranged", L"60 gold",     L"F, T, P, B, L, R",           L"",
    L"E", L"Axe",     L"Mundane", L"Melee",  L"125 gold",    L"F, T, P, B, L, R",           L"",
    L"F", L"Bow",     L"Mundane", L"Ranged", L"350 gold",    L"F, T, P, B, L, R",           L"",
    L"G", L"Sword",   L"Mundane", L"Melee",  L"200 gold",    L"F, T, P, B, L, R",           L"",
    L"H", L"2-H-Swd", L"Mundane", L"Melee",  L"250 gold",    L"F, P, B, L, R",              L"",
    L"I", L"+2 Axe",  L"Magic",   L"Melee",  L"400 gold",    L"F, P, B, L, R",              L"",
    L"J", L"+2 Bow",  L"Magic",   L"Ranged", L"1,050 gold",  L"F, P, B, L, R",              L"",
    L"K", L"+2 Swd",  L"Magic",   L"Melee",  L"800 gold",    L"F, P, B, L, R",              L"",
    L"L", L"Gloves",  L"Magic",   L"Melee",  L"1,200 gold",  L"F, P, B, L",                 L"",
    L"M", L"+4 Axe",  L"Magic",   L"Melee",  L"2,700 gold",  L"F, P, B, L",                 L"",
    L"N", L"+4 Bow",  L"Magic",   L"Ranged", L"6,550 gold",  L"F, P, B, L",                 L"",
    L"O", L"+4 Swd",  L"Magic",   L"Melee",  L"4,550 gold",  L"F, P, B, L",                 L"",
    L"P", L"Exotic",  L"Exotic",  L"Melee",  L"–",           L"All",                        L"Must be found",
};

const Group WEAPON_GROUPS[] = {{nullptr, WEAPON_CELLS, CountOf(WEAPON_CELLS)}};

// Armour -------------------------------------------------------------------

const Column ARMOUR_COLUMNS[] = {
    {L"Key", 40, Align::Left},   {L"Armour", 80, Align::Left},  {L"Kind", 68, Align::Left},
    {L"Price", 74, Align::Right}, {L"Classes", 160, Align::Left}, {L"Notes", 110, Align::Left},
};

const wchar_t* const ARMOUR_CELLS[] = {
    L"A", L"Skin",     L"Mundane", L"–",          L"All",                    L"",
    L"B", L"Cloth",    L"Mundane", L"75 gold",    L"All",                    L"",
    L"C", L"Leather",  L"Mundane", L"195 gold",   L"F, C, T, P, B, I, R",    L"",
    L"D", L"Chain",    L"Mundane", L"575 gold",   L"F, C, P, R",             L"",
    L"E", L"Plate",    L"Mundane", L"2,500 gold", L"F, P, R",                L"",
    L"F", L"+2 Chain", L"Magic",   L"6,130 gold", L"F, R",                   L"",
    L"G", L"+2 Plate", L"Magic",   L"8,250 gold", L"F, R",                   L"",
    L"H", L"Exotic",   L"Exotic",  L"–",          L"All",                    L"Must be found",
};

const Group ARMOUR_GROUPS[] = {{nullptr, ARMOUR_CELLS, CountOf(ARMOUR_CELLS)}};

// Spells -------------------------------------------------------------------

const Column SPELL_COLUMNS[] = {
    {L"Key", 40, Align::Left},     {L"Spell", 96, Align::Left},   {L"MP", 40, Align::Right},
    {L"Effect", 330, Align::Left}, {L"Where", 84, Align::Left},   {L"Cures", 66, Align::Left},
    {L"Classes", 120, Align::Left},
};

const wchar_t* const WIZARD_CELLS[] = {
    L"A", L"Repond",     L"0",  L"May kill each orc, troll or goblin; once per battle",  L"Combat",     L"", L"W, L, D, A, R",
    L"B", L"Mittar",     L"5",  L"Damage one enemy at range",                            L"Combat",     L"", L"W, L, D, A, R",
    L"C", L"Lorum",      L"10", L"Light, like Ignite",                                   L"Anywhere",   L"", L"W, L, D, A, R",
    L"D", L"Dor Acron",  L"15", L"Down one dungeon level, same spot",                    L"Dungeon",    L"", L"W, L, D, A, R",
    L"E", L"Sur Acron",  L"20", L"Up one dungeon level, same spot",                      L"Dungeon",    L"", L"W, L, D, A, R",
    L"F", L"Fulgar",     L"25", L"More damage to one enemy at range",                    L"Combat",     L"", L"W, L, D, A, R",
    L"G", L"Dag Acron",  L"30", L"Teleport to a random spot in the world",               L"Overworld",  L"", L"W, L*, D, A*, R†",
    L"H", L"Mentar",     L"35", L"Damage one enemy at range, based on Int",              L"Combat",     L"", L"W, L*, D, A*, R†",
    L"I", L"Dag Lorum",  L"40", L"Longer-lasting light",                                 L"Anywhere",   L"", L"W, L†, D†, A†",
    L"J", L"Fal Divi",   L"45", L"Cast a cleric spell (its MP is paid as well)",         L"Anywhere",   L"", L"W, L†, D†, A†",
    L"K", L"Noxum",      L"50", L"Damage all enemies",                                   L"Combat",     L"", L"W",
    L"L", L"Decorp",     L"55", L"Try to kill one enemy at range",                       L"Combat",     L"", L"W*",
    L"M", L"Altair",     L"60", L"Stop time, like Negate",                               L"Anywhere",   L"", L"W*",
    L"N", L"Dag Mentar", L"65", L"Damage all enemies, based on Int",                     L"Combat",     L"", L"W*",
    L"O", L"Necorp",     L"70", L"More damage to all enemies",                           L"Combat",     L"", L"W*",
    L"P", L"(no name)",  L"75", L"Try to kill each enemy",                               L"Combat",     L"", L"W*",
};

const wchar_t* const CLERIC_CELLS[] = {
    L"A", L"Pontori",      L"0",  L"May kill each skeleton, ghoul or zombie; once per battle", L"Combat",     L"",         L"C, P, I, D, R",
    L"B", L"Appar Unem",   L"5",  L"Open a chest without setting off its trap",               L"Non-combat", L"",         L"C, P, I, D, R",
    L"C", L"Sanctu",       L"10", L"Heal one party member",                                   L"Anywhere",   L"",         L"C, P, I, D, R",
    L"D", L"Luminae",      L"15", L"Light, like Ignite",                                      L"Anywhere",   L"",         L"C, P, I, D, R",
    L"E", L"Rec Su",       L"20", L"Up one dungeon level, same spot",                         L"Dungeon",    L"",         L"C, P, I, D, R",
    L"F", L"Rec Du",       L"25", L"Down one dungeon level, same spot",                       L"Dungeon",    L"",         L"C, P, I, D, R",
    L"G", L"Lib Rec",      L"30", L"Teleport to a random spot on this dungeon level",         L"Dungeon",    L"",         L"C, P, I, D, R",
    L"H", L"Alcort",       L"35", L"Cure one party member's poison",                          L"Anywhere",   L"Poisoned", L"C, P, I, D, R",
    L"I", L"Sequitu",      L"40", L"Return to the dungeon entrance",                          L"Dungeon",    L"",         L"C, P, I, D",
    L"J", L"Sominae",      L"45", L"Longer-lasting light",                                    L"Anywhere",   L"",         L"C, P, I, D",
    L"K", L"Sanctu Mani",  L"50", L"Heal one party member more",                              L"Anywhere",   L"",         L"C",
    L"L", L"Vieda",        L"55", L"View the map, like Peer",                                 L"Non-combat", L"",         L"C",
    L"M", L"Excuun",       L"60", L"Try to kill one enemy at range",                          L"Combat",     L"",         L"C",
    L"N", L"Surmandum",    L"65", L"Raise the dead; may turn them to ashes instead",          L"Non-combat", L"Dead",     L"C",
    L"O", L"Zxkuqyb",      L"70", L"Try to kill each enemy",                                  L"Combat",     L"",         L"C",
    L"P", L"Anju Sermani", L"75", L"Restore ashes; the caster loses 5 Wis",                   L"Non-combat", L"Ashes",    L"C",
};

const Group SPELL_GROUPS[] = {
    {L"Wizard spells", WIZARD_CELLS, CountOf(WIZARD_CELLS)},
    {L"Cleric spells", CLERIC_CELLS, CountOf(CLERIC_CELLS)},
};

bool PlaceAllows(const wchar_t* where, int map) {
    const bool combat = map == 0x80;
    if (!std::wcscmp(where, L"Combat")) return combat;
    if (!std::wcscmp(where, L"Non-combat")) return !combat;
    if (!std::wcscmp(where, L"Dungeon")) return map == 0x01;
    if (!std::wcscmp(where, L"Overworld")) return map == 0x00;
    return true;  // Anywhere
}

// Whether a Classes cell ("All", or letters like "F, P, B, L*") includes a class.
bool ClassAllowed(const wchar_t* classes, wchar_t classCode) {
    if (!std::wcscmp(classes, L"All")) return true;
    for (const wchar_t* p = classes; *p; ++p)
        if (*p == classCode) return true;
    return false;
}

int SpellCost(const wchar_t* const* spell) { return static_cast<int>(std::wcstol(spell[SPELL_MP], nullptr, 10)); }

// Alive, of a class that has the spell, with the MP for it right now.
bool CanCast(const wchar_t* const* spell, const Caster& caster) {
    return caster.alive && SpellCost(spell) <= caster.mp && ClassAllowed(spell[SPELL_CLASSES], caster.classCode);
}

}  // namespace

const wchar_t* const CLASS_LEGEND = L"F  Fighter\nC  Cleric\nW  Wizard\nT  Thief\nP  Paladin\nB  Barbarian\n"
                                    L"L  Lark\nI  Illusionist\nD  Druid\nA  Alchemist\nR  Ranger";

const Table TABLES[KIND_COUNT] = {
    {L"Weapons — Ultima III Assistant", WEAPON_COLUMNS, CountOf(WEAPON_COLUMNS), WEAPON_GROUPS,
     CountOf(WEAPON_GROUPS),
     L"Key: the letter to press when you Ready a weapon. Magic weapons are sold only in Dawn; shops buy any "
     L"weapon they sell for its full price.",
     1, 5, L"ready"},
    {L"Armour — Ultima III Assistant", ARMOUR_COLUMNS, CountOf(ARMOUR_COLUMNS), ARMOUR_GROUPS, CountOf(ARMOUR_GROUPS),
     L"Key: the letter to press when you Wear armour. Magic armour is sold only in Dawn; shops buy any armour "
     L"they sell for its full price.",
     1, 4, L"wear"},
    {L"Spells — Ultima III Assistant", SPELL_COLUMNS, CountOf(SPELL_COLUMNS), SPELL_GROUPS, CountOf(SPELL_GROUPS),
     L"Key: the letter to press when you Cast. MP is the cost. † only for some races; * for all but one race.",
     SPELL_NAME, SPELL_CLASSES, L"cast"},
};

int Table::RowCount() const {
    int rows = 0;
    for (int g = 0; g < groupCount; ++g) rows += groups[g].cellCount / columnCount;
    return rows;
}

const wchar_t* const* Table::Row(int row) const {
    if (row < 0) return nullptr;
    for (int g = 0; g < groupCount; ++g) {
        const int rows = groups[g].cellCount / columnCount;
        if (row < rows) return &groups[g].cells[row * columnCount];
        row -= rows;
    }
    return nullptr;
}

PartyState PartyStateOf(const Party* party, int combatTurn) {
    PartyState state;
    if (!party) return state;
    state.live = true;
    state.map = party->map;
    state.combatTurn = combatTurn;
    state.count = party->count < 4 ? party->count : 4;
    for (int i = 0; i < state.count; ++i) {
        const Character& ch = party->chars[i];
        Caster& caster = state.members[i];
        caster.name = ch.name;
        caster.classCode = static_cast<wchar_t>(static_cast<unsigned char>(ch.classCode));
        caster.alive = ch.present && (ch.statusCode == 'G' || ch.statusCode == 'P');
        caster.mp = ch.mp;
    }
    return state;
}

std::vector<bool> CastableRows(const PartyState& party, std::wstring* whom) {
    const Table& table = TABLES[SPELLS];
    const bool single = party.map == 0x80 && party.combatTurn >= 0 && party.combatTurn < party.count;
    *whom = single ? L"Castable now by " + party.members[party.combatTurn].name
                   : std::wstring(L"Castable now by someone in the party");

    std::vector<bool> rows;
    for (int row = 0; row < table.RowCount(); ++row) {
        const wchar_t* const* spell = table.Row(row);
        bool castable = false;
        if (PlaceAllows(spell[SPELL_WHERE], party.map))
            for (int m = 0; m < party.count && m < 4 && !castable; ++m)
                castable = (!single || m == party.combatTurn) && CanCast(spell, party.members[m]);
        rows.push_back(castable);
    }
    return rows;
}

std::wstring UsersText(Kind kind, int row, const PartyState& party) {
    const Table& table = TABLES[kind];
    const wchar_t* const* cells = table.Row(row);
    if (!cells) return L"";
    if (!party.live) return L"No party connected.";

    const std::wstring name = cells[table.nameColumn];
    std::wstring users;
    for (int m = 0; m < party.count && m < 4; ++m) {
        const Caster& member = party.members[m];
        if (!ClassAllowed(cells[table.classesColumn], member.classCode)) continue;
        users += L"\n" + member.name;
        if (kind == SPELLS) {
            const int cost = SpellCost(cells);
            if (!member.alive)
                users += L" (not alive)";
            else if (member.mp < cost)
                users += L" (has " + std::to_wstring(member.mp) + L" of " + std::to_wstring(cost) + L" MP)";
        }
    }
    if (users.empty()) return L"No one in the party can " + std::wstring(table.verb) + L" " + name + L".";
    return L"Can " + std::wstring(table.verb) + L" " + name + L":" + users;
}

}  // namespace u3::ref
