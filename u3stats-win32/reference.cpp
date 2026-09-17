// reference.cpp -- Weapons, Armour and Spells reference windows, showing the
// tables in core/refdata.
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

using u3::ref::Align;
using u3::ref::CLASS_LEGEND;
using u3::ref::Group;
using u3::ref::Table;
using u3::ref::TABLES;

HINSTANCE g_inst;
HFONT g_font;
int g_dpi = 96;
HWND g_windows[KIND_COUNT];

// Settings keys: window placement, and whether the window was open at exit.
const wchar_t* const PLACEMENT_KEYS[KIND_COUNT] = {L"Weapons", L"Armour", L"Spells"};
const wchar_t* const OPEN_KEYS[KIND_COUNT] = {L"WeaponsOpen", L"ArmourOpen", L"SpellsOpen"};
bool g_appClosing = false;  // once set, windows closing no longer count as closed by the user
// When each window was last closed by the user. Windows' taskbar "Close all
// windows" closes these just before the main window, so a window closed that
// recently still counts as open when the app quits.
ULONGLONG g_closedAt[KIND_COUNT];
constexpr ULONGLONG CLOSE_ALL_GRACE_MS = 2000;

PartyState g_party;
std::vector<bool> g_spellRows;  // spells shown by the filter; empty when not filtering

// Each window's tooltips: the class legend on the Classes header, and who can
// use the row under the mouse.
struct Tips {
    HWND tip = nullptr, list = nullptr, header = nullptr;
    int row = -1;       // table row under the mouse, or -1
    std::wstring text;  // kept alive for TTN_GETDISPINFO
} g_tips[KIND_COUNT];

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
    const bool grouped = table.Grouped();
    int item = 0, row = 0;
    for (int g = 0; g < table.groupCount; ++g) {
        const Group& group = table.groups[g];
        for (int cell = 0; cell < group.cellCount; cell += table.columnCount, ++row) {
            if (visible && !(*visible)[row]) continue;
            LVITEMW entry{};
            entry.mask = LVIF_TEXT | LVIF_PARAM | (grouped ? LVIF_GROUPID : 0);
            entry.iItem = item;
            entry.iGroupId = g;
            entry.lParam = row;
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
        col.fmt = table.columns[c].align == Align::Right ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx = S(table.columns[c].width);
        col.pszText = const_cast<LPWSTR>(table.columns[c].title);
        ListView_InsertColumn(list, c, &col);
    }

    if (table.Grouped()) {
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

void ApplySpellFilter(bool force) {
    HWND hwnd = g_windows[SPELLS];
    if (!hwnd) return;
    const bool on = SendMessageW(GetDlgItem(hwnd, IDC_CASTABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring caption;
    std::vector<bool> rows;
    if (on && g_party.live)
        rows = u3::ref::CastableRows(g_party, &caption);
    else if (on)
        caption = L"No party connected, so every spell is shown";
    SetText(GetDlgItem(hwnd, IDC_CASTABLE_FOR), caption);

    if (force || rows != g_spellRows) {
        g_spellRows = rows;
        FillRows(GetDlgItem(hwnd, IDC_LIST), TABLES[SPELLS], rows.empty() ? nullptr : &rows);
    }
}

// Tooltips -------------------------------------------------------------------

void RelayToTip(HWND tip, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_MOUSEMOVE && (msg < WM_LBUTTONDOWN || msg > WM_MBUTTONDBLCLK)) return;
    MSG m{};
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wp;
    m.lParam = lp;
    m.time = GetMessageTime();
    const DWORD pos = GetMessagePos();
    m.pt = {static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos))};
    SendMessageW(tip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&m));
}

// Keeps the legend tool over the Classes column as columns are resized.
LRESULT CALLBACK HeaderSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    const Tips& tips = g_tips[ref];
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, HeaderSubclass, 0);
    } else if (tips.tip && msg == WM_MOUSEMOVE) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof tool;
        tool.hwnd = hwnd;
        tool.uId = 1;
        Header_GetItemRect(hwnd, TABLES[ref].classesColumn, &tool.rect);
        SendMessageW(tips.tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    if (tips.tip) RelayToTip(tips.tip, hwnd, msg, wp, lp);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Tracks the row under the mouse, restarting the tooltip when it changes.
LRESULT CALLBACK ListSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    Tips& tips = g_tips[ref];
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ListSubclass, 0);
    } else if (tips.tip && msg == WM_MOUSEMOVE) {
        LVHITTESTINFO hit{};
        hit.pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        int row = -1;
        if (ListView_SubItemHitTest(hwnd, &hit) >= 0 && hit.iItem >= 0) {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = hit.iItem;
            if (ListView_GetItem(hwnd, &item)) row = static_cast<int>(item.lParam);
        }
        if (row != tips.row) {
            tips.row = row;
            SendMessageW(tips.tip, TTM_ACTIVATE, FALSE, 0);
            SendMessageW(tips.tip, TTM_ACTIVATE, TRUE, 0);
        }
    }
    if (tips.tip) RelayToTip(tips.tip, hwnd, msg, wp, lp);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void CreateTips(Kind kind, HWND hwnd, HWND list) {
    Tips& tips = g_tips[kind];
    tips = Tips{};
    tips.list = list;
    tips.header = ListView_GetHeader(list);
    tips.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0,
                               0, 0, hwnd, nullptr, g_inst, nullptr);
    if (!tips.tip) return;
    SendMessageW(tips.tip, TTM_SETMAXTIPWIDTH, 0, S(360));  // also lets the text break at \n
    SendMessageW(tips.tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

    TOOLINFOW legend{};
    legend.cbSize = sizeof legend;
    legend.hwnd = tips.header;
    legend.uId = 1;
    legend.lpszText = const_cast<LPWSTR>(CLASS_LEGEND);
    Header_GetItemRect(tips.header, TABLES[kind].classesColumn, &legend.rect);
    SendMessageW(tips.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&legend));

    TOOLINFOW rows{};
    rows.cbSize = sizeof rows;
    rows.uFlags = TTF_IDISHWND;
    rows.hwnd = hwnd;
    rows.uId = reinterpret_cast<UINT_PTR>(list);
    rows.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tips.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&rows));

    SetWindowSubclass(tips.header, HeaderSubclass, 0, kind);
    SetWindowSubclass(list, ListSubclass, 0, kind);
}

LRESULT CALLBACK ReferenceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            LayoutWindow(hwnd);
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->code != TTN_GETDISPINFOW) break;
            for (int kind = 0; kind < KIND_COUNT; ++kind) {
                Tips& tips = g_tips[kind];
                if (g_windows[kind] != hwnd || hdr->hwndFrom != tips.tip) continue;
                tips.text = u3::ref::UsersText(static_cast<Kind>(kind), tips.row, g_party);
                reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText = const_cast<LPWSTR>(tips.text.c_str());
                return 0;
            }
            break;
        }
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
                    g_closedAt[kind] = GetTickCount64();
                }
                g_windows[kind] = nullptr;
                g_tips[kind] = Tips{};  // the tooltip goes with its owner window
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
    const bool grouped = table.Grouped();
    int clientW = GetSystemMetrics(SM_CXVSCROLL) + S(6);
    for (int c = 0; c < table.columnCount; ++c) clientW += S(table.columns[c].width);
    const int rows = table.RowCount();
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
    // Recorded now, so it's reopened next time even if the app never gets to close normally.
    settings::SetInt(L"Reference", OPEN_KEYS[kind], 1);
    g_closedAt[kind] = 0;

    if (kind == SPELLS) {
        ChildControl(hwnd, L"BUTTON", L"&Castable only", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CASTABLE);
        ChildControl(hwnd, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX | SS_CENTERIMAGE,
                     IDC_CASTABLE_FOR);
        g_spellRows.clear();
    }
    CreateTips(kind, hwnd, CreateTable(hwnd, table));
    ChildControl(hwnd, L"STATIC", table.note, SS_LEFT | SS_NOPREFIX, IDC_NOTE);
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
    const ULONGLONG now = GetTickCount64();
    for (int kind = 0; kind < KIND_COUNT; ++kind) {
        if (g_windows[kind]) settings::SaveWindow(PLACEMENT_KEYS[kind], g_windows[kind]);
        const bool justClosed = g_closedAt[kind] && now - g_closedAt[kind] < CLOSE_ALL_GRACE_MS;
        settings::SetInt(L"Reference", OPEN_KEYS[kind], g_windows[kind] || justClosed ? 1 : 0);
    }
}

}  // namespace u3ref
