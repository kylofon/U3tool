// reference.cpp -- Weapons, Armour and Spells reference windows.
//
// The tables come from the Gamer Corner Ultima III guide
// (https://guides.gamercorner.net/ultimaiii/). Their class columns agree with
// the limits EXODUS.BIN enforces, which reader.cpp uses for equipping.
#include "reference.h"

#include "settings.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace u3ref {
namespace {

constexpr int IDC_LIST = 1, IDC_NOTE = 2, IDC_CASTABLE = 3, IDC_CASTABLE_FOR = 4;
const wchar_t* const CLASS_NAME = L"U3AssistantReference";

struct Column {
    const wchar_t* title;
    int width;  // at 96 dpi
    int format;
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
};

template <typename T, size_t N>
constexpr int CountOf(const T (&)[N]) { return static_cast<int>(N); }

const wchar_t* const CLASS_LEGEND =
    L"Classes: F Fighter, C Cleric, W Wizard, T Thief, P Paladin, B Barbarian, L Lark, I Illusionist, D Druid, "
    L"A Alchemist, R Ranger.";

// Weapons ------------------------------------------------------------------

const Column WEAPON_COLUMNS[] = {
    {L"Key", 40, LVCFMT_LEFT},    {L"Weapon", 80, LVCFMT_LEFT},    {L"Kind", 68, LVCFMT_LEFT},
    {L"Range", 62, LVCFMT_LEFT},  {L"Price", 70, LVCFMT_RIGHT},    {L"Classes", 150, LVCFMT_LEFT},
    {L"Notes", 130, LVCFMT_LEFT},
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
    {L"Key", 40, LVCFMT_LEFT},   {L"Armour", 80, LVCFMT_LEFT},  {L"Kind", 68, LVCFMT_LEFT},
    {L"Price", 74, LVCFMT_RIGHT}, {L"Classes", 160, LVCFMT_LEFT}, {L"Notes", 110, LVCFMT_LEFT},
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

enum SpellColumn { SPELL_KEY, SPELL_NAME, SPELL_MP, SPELL_EFFECT, SPELL_WHERE, SPELL_CURES, SPELL_CLASSES };

const Column SPELL_COLUMNS[] = {
    {L"Key", 40, LVCFMT_LEFT},     {L"Spell", 96, LVCFMT_LEFT},   {L"MP", 40, LVCFMT_RIGHT},
    {L"Effect", 330, LVCFMT_LEFT}, {L"Where", 84, LVCFMT_LEFT},   {L"Cures", 66, LVCFMT_LEFT},
    {L"Classes", 120, LVCFMT_LEFT},
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

const Table TABLES[KIND_COUNT] = {
    {L"Weapons — Ultima III Assistant", WEAPON_COLUMNS, CountOf(WEAPON_COLUMNS), WEAPON_GROUPS,
     CountOf(WEAPON_GROUPS),
     L"Key: the letter to press when you Ready a weapon. Magic weapons are sold only in Dawn; shops buy any "
     L"weapon they sell for its full price. "},
    {L"Armour — Ultima III Assistant", ARMOUR_COLUMNS, CountOf(ARMOUR_COLUMNS), ARMOUR_GROUPS, CountOf(ARMOUR_GROUPS),
     L"Key: the letter to press when you Wear armour. Magic armour is sold only in Dawn; shops buy any armour "
     L"they sell for its full price. "},
    {L"Spells — Ultima III Assistant", SPELL_COLUMNS, CountOf(SPELL_COLUMNS), SPELL_GROUPS, CountOf(SPELL_GROUPS),
     L"Key: the letter to press when you Cast. MP is the cost. † only for some races; * for all but one race. "},
};

HINSTANCE g_inst;
HFONT g_font;
int g_dpi = 96;
HWND g_windows[KIND_COUNT];

// Settings keys: window placement, and whether the window was open at exit.
const wchar_t* const PLACEMENT_KEYS[KIND_COUNT] = {L"Weapons", L"Armour", L"Spells"};
const wchar_t* const OPEN_KEYS[KIND_COUNT] = {L"WeaponsOpen", L"ArmourOpen", L"SpellsOpen"};
bool g_appClosing = false;  // once set, windows closing no longer count as closed by the user

PartyState g_party;
std::vector<bool> g_spellRows;  // spells shown by the filter; empty when not filtering

int S(int v) { return MulDiv(v, g_dpi, 96); }

int LineHeight() {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight + S(4);
}

int NoteHeight() { return 3 * LineHeight(); }
int FilterHeight() { return LineHeight() + S(10); }  // the Spells window's checkbox row

void SetText(HWND h, const std::wstring& text) {
    int len = GetWindowTextLengthW(h);
    std::wstring current(len + 1, L'\0');
    GetWindowTextW(h, &current[0], len + 1);
    current.resize(len);
    if (current != text) SetWindowTextW(h, text.c_str());
}

void LayoutWindow(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int noteH = NoteHeight();
    int top = 0;
    if (HWND check = GetDlgItem(hwnd, IDC_CASTABLE)) {
        top = FilterHeight();
        const int checkW = S(120), rowH = LineHeight();
        MoveWindow(check, S(8), (top - rowH) / 2, checkW, rowH, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_CASTABLE_FOR), S(8) + checkW + S(8), (top - rowH) / 2,
                   std::max<int>(rc.right - checkW - S(24), 0), rowH, TRUE);
    }
    MoveWindow(GetDlgItem(hwnd, IDC_LIST), 0, top, rc.right, std::max<int>(rc.bottom - noteH - top, 0), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_NOTE), S(8), rc.bottom - noteH + S(4), std::max<int>(rc.right - S(16), 0),
               noteH - S(6), TRUE);
}

// Fills the list with the table's rows, or only those marked in `visible`.
void FillRows(HWND list, const Table& table, const std::vector<bool>* visible) {
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    const bool grouped = table.groups[0].title != nullptr;
    int item = 0, row = 0;
    for (int g = 0; g < table.groupCount; ++g) {
        const Group& group = table.groups[g];
        for (int cell = 0; cell < group.cellCount; cell += table.columnCount, ++row) {
            if (visible && !(*visible)[row]) continue;
            LVITEMW entry{};
            entry.mask = LVIF_TEXT | (grouped ? LVIF_GROUPID : 0);
            entry.iItem = item;
            entry.iGroupId = g;
            entry.pszText = const_cast<LPWSTR>(group.cells[cell]);
            ListView_InsertItem(list, &entry);
            for (int c = 1; c < table.columnCount; ++c)
                ListView_SetItemText(list, item, c, const_cast<LPWSTR>(group.cells[cell + c]));
            ++item;
        }
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

HWND CreateTable(HWND parent, const Table& table) {
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 0, 0, 0, 0,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), g_inst, nullptr);
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    for (int c = 0; c < table.columnCount; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = table.columns[c].format;
        col.cx = S(table.columns[c].width);
        col.pszText = const_cast<LPWSTR>(table.columns[c].title);
        ListView_InsertColumn(list, c, &col);
    }

    if (table.groups[0].title) {
        ListView_EnableGroupView(list, TRUE);
        for (int g = 0; g < table.groupCount; ++g) {
            LVGROUP header{};
            header.cbSize = sizeof header;
            header.mask = LVGF_HEADER | LVGF_GROUPID;
            header.pszHeader = const_cast<LPWSTR>(table.groups[g].title);
            header.iGroupId = g;
            ListView_InsertGroup(list, -1, &header);
        }
    }
    FillRows(list, table, nullptr);
    return list;
}

// "Castable only" -----------------------------------------------------------

bool PlaceAllows(const wchar_t* where, int map) {
    const bool combat = map == 0x80;
    if (!std::wcscmp(where, L"Combat")) return combat;
    if (!std::wcscmp(where, L"Non-combat")) return !combat;
    if (!std::wcscmp(where, L"Dungeon")) return map == 0x01;
    if (!std::wcscmp(where, L"Overworld")) return map == 0x00;
    return true;  // Anywhere
}

// Alive, of a class that has the spell, with the MP for it right now.
bool CanCast(const wchar_t* const* spell, const Caster& caster) {
    if (!caster.alive || _wtoi(spell[SPELL_MP]) > caster.mp) return false;
    for (const wchar_t* p = spell[SPELL_CLASSES]; *p; ++p)
        if (*p == caster.classCode) return true;
    return false;
}

// Which spells to show: in combat, those the member whose turn it is can cast;
// elsewhere, those anyone in the party can. `whom` gets a caption.
std::vector<bool> CastableRows(std::wstring* whom) {
    const Table& table = TABLES[SPELLS];
    const PartyState& party = g_party;
    const bool single = party.map == 0x80 && party.combatTurn >= 0 && party.combatTurn < party.count;
    *whom = single ? L"Castable now by " + party.members[party.combatTurn].name
                   : std::wstring(L"Castable now by someone in the party");

    std::vector<bool> rows;
    for (int g = 0; g < table.groupCount; ++g) {
        const Group& group = table.groups[g];
        for (int cell = 0; cell < group.cellCount; cell += table.columnCount) {
            const wchar_t* const* spell = &group.cells[cell];
            bool castable = false;
            if (PlaceAllows(spell[SPELL_WHERE], party.map))
                for (int m = 0; m < party.count && m < 4 && !castable; ++m)
                    castable = (!single || m == party.combatTurn) && CanCast(spell, party.members[m]);
            rows.push_back(castable);
        }
    }
    return rows;
}

void ApplySpellFilter(bool force) {
    HWND hwnd = g_windows[SPELLS];
    if (!hwnd) return;
    const bool on = SendMessageW(GetDlgItem(hwnd, IDC_CASTABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring caption;
    std::vector<bool> rows;
    if (on && g_party.live)
        rows = CastableRows(&caption);
    else if (on)
        caption = L"No party connected, so every spell is shown";
    SetText(GetDlgItem(hwnd, IDC_CASTABLE_FOR), caption);

    if (force || rows != g_spellRows) {
        g_spellRows = rows;
        FillRows(GetDlgItem(hwnd, IDC_LIST), TABLES[SPELLS], rows.empty() ? nullptr : &rows);
    }
}

LRESULT CALLBACK ReferenceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            LayoutWindow(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_CASTABLE && HIWORD(wp) == BN_CLICKED) {
                ApplySpellFilter(true);
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = S(260);
            mm->ptMinTrackSize.y = S(180);
            return 0;
        }
        case WM_DESTROY:
            for (int kind = 0; kind < KIND_COUNT; ++kind) {
                if (g_windows[kind] != hwnd) continue;
                if (!g_appClosing) {
                    settings::SaveWindow(PLACEMENT_KEYS[kind], hwnd);
                    settings::SetInt(L"Reference", OPEN_KEYS[kind], 0);
                }
                g_windows[kind] = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND ChildControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    return h;
}

}  // namespace

void Register(HINSTANCE inst, HICON icon, HICON smallIcon) {
    g_inst = inst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ReferenceProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = icon;
    wc.hIconSm = smallIcon;
    RegisterClassExW(&wc);
}

void Show(Kind kind, HWND owner, HFONT font, int dpi, bool topmost) {
    g_font = font;
    g_dpi = dpi;
    if (HWND open = g_windows[kind]) {
        if (IsIconic(open)) ShowWindow(open, SW_RESTORE);
        SetForegroundWindow(open);
        return;
    }

    // Size the window to its columns and rows, capped so it fits beside others.
    const Table& table = TABLES[kind];
    const bool grouped = table.groups[0].title != nullptr;
    int clientW = GetSystemMetrics(SM_CXVSCROLL) + S(6);
    for (int c = 0; c < table.columnCount; ++c) clientW += S(table.columns[c].width);
    int rows = 0;
    for (int g = 0; g < table.groupCount; ++g) rows += table.groups[g].cellCount / table.columnCount;
    const int line = LineHeight();
    const int listH = std::min(line + S(8) + (rows + (grouped ? 2 * table.groupCount : 0)) * (line + S(1)), S(560));
    const int filterH = kind == SPELLS ? FilterHeight() : 0;

    const DWORD style = WS_OVERLAPPEDWINDOW, exStyle = topmost ? WS_EX_TOPMOST : 0;
    RECT r{0, 0, clientW, filterH + listH + NoteHeight()};
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    const int w = r.right - r.left, h = r.bottom - r.top;

    // Cascade from the assistant's top-left corner, kept on its monitor.
    RECT anchor;
    GetWindowRect(owner, &anchor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    const int step = S(40) * (kind + 1);
    const int x = std::max<int>(work.left, std::min<int>(anchor.left + step, work.right - w));
    const int y = std::max<int>(work.top, std::min<int>(anchor.top + step, work.bottom - h));

    HWND hwnd = CreateWindowExW(exStyle, CLASS_NAME, table.title, style, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!hwnd) return;
    g_windows[kind] = hwnd;

    if (kind == SPELLS) {
        ChildControl(hwnd, L"BUTTON", L"&Castable only", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CASTABLE);
        ChildControl(hwnd, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX | SS_CENTERIMAGE,
                     IDC_CASTABLE_FOR);
        g_spellRows.clear();
    }
    CreateTable(hwnd, table);
    ChildControl(hwnd, L"STATIC", (std::wstring(table.note) + CLASS_LEGEND).c_str(), SS_LEFT | SS_NOPREFIX,
                 IDC_NOTE);
    LayoutWindow(hwnd);
    if (!settings::RestoreWindow(PLACEMENT_KEYS[kind], hwnd)) ShowWindow(hwnd, SW_SHOWNORMAL);
}

void SetTopmost(bool topmost) {
    for (HWND w : g_windows)
        if (w)
            SetWindowPos(w, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void UpdateParty(const PartyState& party) {
    g_party = party;
    ApplySpellFilter(false);
}

void RestoreOpenWindows(HWND owner, HFONT font, int dpi, bool topmost) {
    for (int kind = 0; kind < KIND_COUNT; ++kind)
        if (settings::GetInt(L"Reference", OPEN_KEYS[kind], 0)) Show(static_cast<Kind>(kind), owner, font, dpi, topmost);
}

void SaveOpenWindows() {
    g_appClosing = true;
    for (int kind = 0; kind < KIND_COUNT; ++kind) {
        if (g_windows[kind]) settings::SaveWindow(PLACEMENT_KEYS[kind], g_windows[kind]);
        settings::SetInt(L"Reference", OPEN_KEYS[kind], g_windows[kind] ? 1 : 0);
    }
}

}  // namespace u3ref
