// main.cpp -- Ultima III Assistant: a native Win32 live party viewer and editor.
//
// A worker thread polls DOSBox (Staging's HTTP API, or the process's memory)
// through u3::DosBoxReader and posts snapshots
// to the window; the UI thread decodes them and updates standard controls,
// touching only what actually changed so nothing flickers.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

#include "reader.h"
#include "crash.h"
#include "maps.h"
#include "reference.h"
#include "settings.h"
#include "version.h"

namespace {

constexpr UINT WM_APP_SNAPSHOT = WM_APP + 1;
constexpr UINT WM_APP_ACTION = WM_APP + 2;
constexpr UINT WM_APP_DROP = WM_APP + 3;

const wchar_t* const APP_TITLE = L"Ultima III Assistant";

enum : int { IDC_RAWEDIT = 100, IDC_MINUS, IDC_PLUS, IDC_COUNT, IDC_TOPMOST };
enum : int {
    IDM_QUIT = 200,
    IDM_FOOD,
    IDM_FASTER,
    IDM_SLOWER,
    IDM_PAUSE,
    IDM_NORMAL,
    IDM_PREFERENCES,
    IDM_RESCAN,
    IDM_NEXT,
    IDM_RAW,
    IDM_POOL_BASE = 210,  // IDM_POOL_BASE + party member
    IDM_EQUIP = 220,
    IDM_DEBUG_INFO,
    IDM_ABOUT,
    IDM_REFERENCE_BASE = 230,  // IDM_REFERENCE_BASE + u3ref::Kind
    IDM_REVIVE_BASE = 240,     // IDM_REVIVE_BASE + party member
    IDM_HEAL_BASE = 250,       // IDM_HEAL_BASE + party member
    IDM_CURE_BASE = 260,       // IDM_CURE_BASE + party member
    IDM_MAPS_BASE = 270,       // IDM_MAPS_BASE + u3maps::Kind
};

// Requests handed to the worker thread; pooling, reviving, healing and curing
// add the member index, and an item move or equipment change packs its details
// into the low bits.
constexpr int ACTION_FOOD = 1;
constexpr int ACTION_POOL = 10;
constexpr int ACTION_REVIVE = 20;
constexpr int ACTION_HEAL = 30;
constexpr int ACTION_CURE = 40;
constexpr int ACTION_MOVE = 1 << 16;
constexpr int ACTION_EQUIP = 1 << 17;

int PackMove(const u3::ItemMove& m) {
    return ACTION_MOVE | m.from << 14 | m.to << 12 | (m.armour ? 1 : 0) << 11 | m.type << 7 | m.count;
}

u3::ItemMove UnpackMove(int action) {
    u3::ItemMove m;
    m.from = action >> 14 & 3;
    m.to = action >> 12 & 3;
    m.armour = (action >> 11 & 1) != 0;
    m.type = action >> 7 & 15;
    m.count = action & 127;
    return m;
}

int PackEquip(const u3::Equip& e) { return ACTION_EQUIP | e.member << 5 | (e.armour ? 1 : 0) << 4 | e.type; }

u3::Equip UnpackEquip(int action) {
    u3::Equip e;
    e.member = action >> 5 & 3;
    e.armour = (action >> 4 & 1) != 0;
    e.type = action & 15;
    return e;
}

// Equipment has no row: the Carrying list marks what's readied and worn.
enum Row { ROW_HP, ROW_MP, ROW_EXP, ROW_FOOD, ROW_GOLD, ROW_COUNT };
const wchar_t* const ROW_LABELS[ROW_COUNT] = {L"Hit points", L"Magic points", L"Experience", L"Food", L"Gold"};
const wchar_t* const STAT_LABELS[4] = {L"Strength", L"Dexterity", L"Intelligence", L"Wisdom"};
const wchar_t* const COUNTER_LABELS[4] = {L"Gems", L"Keys", L"Powders", L"Torches"};

const COLORREF COL_GOOD = RGB(0, 128, 0);
const COLORREF COL_POISONED = RGB(170, 110, 0);
const COLORREF COL_DEAD = RGB(192, 0, 0);
const COLORREF COL_ASHES = RGB(110, 110, 110);

// Idle waits offered by the Game speed menu, fastest first.
const int PASS_STEPS[] = {1, 2, 3, u3::NORMAL_PASS_SECONDS, 10, 15, 30, u3::MAX_PASS_SECONDS};
constexpr int STEP_COUNT = sizeof PASS_STEPS / sizeof PASS_STEPS[0];
constexpr int NORMAL_STEP = 3;

struct Snapshot {
    bool ok = false;
    u3::PartyBytes raw{};
    std::wstring error, exe;
    DWORD pid = 0;
    uint64_t address = 0;
    size_t candidates = 0, index = 0;
    u3::SpeedState speed;
    int combatTurn = -1;
    bool hasLocation = false;
    u3::Location location;
    std::wstring gameFolder;
};

struct CharCtl {
    HWND group, name, kind, status, hpBar, carryLbl, carryList;
    HWND rowLbl[ROW_COUNT], rowVal[ROW_COUNT];
    HWND statLbl[4], statVal[4];
    HWND cntLbl[4], cntVal[4];
    HWND sep[3];
    COLORREF statusColor = COL_GOOD;
    bool empty = true;
    int barMax = -1, barPos = -1, barState = -1;
    bool levelUp = false;             // Lord British would raise max HP
    std::vector<std::wstring> carried;
    std::vector<u3::CarriedItem> items;  // what each line of carryList holds
};

HINSTANCE g_inst;
HWND g_hwnd, g_rawEdit, g_statusBar;
HWND g_waitLabel;          // shown in place of the party while not connected
bool g_partyShown = true;  // whether the party columns are visible
HMENU g_actionsMenu, g_poolMenu, g_speedMenu, g_debugMenu, g_cheatMenu, g_reviveMenu, g_healMenu, g_cureMenu;
CharCtl g_chars[4];
HFONT g_font, g_fontBold, g_fontName, g_fontMono;
int g_dpi = 96, g_lineH = 18, g_nameH = 24;
int g_columnH = 0;  // height of a party member's box, set by Layout
bool g_showRaw = false;
bool g_topmost = true;

// Status bar: the latest message on the left, game speed on the right.
std::wstring g_statusParts[2] = {L"Ready.", L""};
std::wstring g_problem;  // why we're not connected, as last shown

// Connection details for the Debug menu.
std::wstring g_debugInfo = L"Not connected";
size_t g_sourceCount = 0, g_sourceIndex = 0;  // copies of the party block found, and the one shown
bool g_haveRaw = false;
u3::PartyBytes g_lastRaw{};
std::wstring g_lastHex;

bool g_live = false;
u3::Party g_party;  // last decoded party, for building the Pool gold menu

// Dragging an item between carried lists.
UINT g_dragListMsg;
int g_dragFrom = -1, g_dropTo = -1;
u3::CarriedItem g_dragItem;

std::atomic<bool> g_wantRescan{false}, g_wantNext{false};
std::atomic<int> g_action{0};

// Game speed as chosen in the menu (UI thread), mirrored for the worker.
int g_step = NORMAL_STEP;
bool g_paused = false;
std::atomic<int> g_wantSeconds{u3::NORMAL_PASS_SECONDS};
std::atomic<bool> g_wantPaused{false};
HANDLE g_stopEvent, g_wakeEvent, g_thread;

int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    return buf;
}

std::wstring Num(int v) { return v < 0 ? std::wstring(L"–") : std::to_wstring(v); }

std::wstring Hex64(uint64_t v) {
    DWORD hi = static_cast<DWORD>(v >> 32), lo = static_cast<DWORD>(v);
    return hi ? Format(L"0x%lX%08lX", hi, lo) : Format(L"0x%lX", lo);
}

void SetText(HWND h, const std::wstring& text) {
    int len = GetWindowTextLengthW(h);
    std::wstring current(len + 1, L'\0');
    GetWindowTextW(h, &current[0], len + 1);
    current.resize(len);
    if (current != text) SetWindowTextW(h, text.c_str());
}

bool IsChecked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

int FontHeight(HFONT font) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

void CreateFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);

    LOGFONTW lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 3 / 2;
    g_fontName = CreateFontIndirectW(&lf);

    LOGFONTW mono = ncm.lfMessageFont;
    lstrcpynW(mono.lfFaceName, L"Consolas", LF_FACESIZE);
    g_fontMono = CreateFontIndirectW(&mono);

    g_lineH = FontHeight(g_font) + S(4);
    g_nameH = FontHeight(g_fontName) + S(4);
}

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, HFONT font, int id = 0, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return h;
}

HWND Label(const wchar_t* text, HFONT font, DWORD type = SS_LEFTNOWORDWRAP) {
    return Child(L"STATIC", text, SS_NOPREFIX | type, font);
}

void CreateControls() {
    for (int i = 0; i < 4; ++i) {
        CharCtl& c = g_chars[i];
        c.name = Label(L"", g_fontName, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
        c.kind = Label(L"", g_font, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
        c.status = Label(L"", g_font, SS_RIGHT);
        c.hpBar = Child(PROGRESS_CLASSW, L"", 0, g_font);
        for (int r = 0; r < ROW_COUNT; ++r) {
            c.rowLbl[r] = Label(ROW_LABELS[r], g_font);
            c.rowVal[r] = Label(L"", g_fontBold, SS_RIGHT | (r == ROW_EXP ? SS_NOTIFY : 0));  // notify: for its tooltip
        }
        for (int k = 0; k < 4; ++k) {
            c.statLbl[k] = Label(STAT_LABELS[k], g_font);
            c.statVal[k] = Label(L"", g_fontBold, SS_RIGHT);
            c.cntLbl[k] = Label(COUNTER_LABELS[k], g_font);
            c.cntVal[k] = Label(L"", g_fontBold, SS_RIGHT);
        }
        for (HWND& sep : c.sep) sep = Child(L"STATIC", L"", SS_ETCHEDHORZ, g_font);
        c.carryLbl = Label(L"Carrying", g_font);
        // Owner drawn so equipped items can be shown in bold.
        c.carryList = Child(L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL,
                            g_font, 0, WS_EX_CLIENTEDGE);
        MakeDragList(c.carryList);  // items can be dragged onto another member

        // The group box goes to the bottom of the z-order and clips its
        // siblings, so it never paints over the controls it frames.
        c.group = Child(L"BUTTON", L"", BS_GROUPBOX | WS_CLIPSIBLINGS, g_font);
        SetWindowPos(c.group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Explains the ▲ that marks someone Lord British will raise.
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, g_hwnd,
                               nullptr, g_inst, nullptr);
    for (CharCtl& c : g_chars) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof tool;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = g_hwnd;
        tool.uId = reinterpret_cast<UINT_PTR>(c.rowVal[ROW_EXP]);
        tool.lpszText = const_cast<LPWSTR>(L"▲ Lord British will raise this character's maximum hit points by 100 — "
                                           L"visit him.");
        SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    g_rawEdit = Child(L"EDIT", L"",
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                      g_fontMono, IDC_RAWEDIT, WS_EX_CLIENTEDGE);
    ShowWindow(g_rawEdit, SW_HIDE);

    g_waitLabel = Label(L"Waiting for connection, is the game running?", g_fontName, SS_CENTER | SS_CENTERIMAGE);

    g_statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                  g_hwnd, nullptr, g_inst, nullptr);
}

// Shows the party columns while connected; otherwise only the waiting message.
void ShowParty(bool show) {
    if (show == g_partyShown) return;
    g_partyShown = show;
    const int cmd = show ? SW_SHOW : SW_HIDE;
    for (CharCtl& c : g_chars) {
        for (HWND h : {c.group, c.name, c.kind, c.status, c.hpBar, c.carryLbl, c.carryList}) ShowWindow(h, cmd);
        for (int r = 0; r < ROW_COUNT; ++r) {
            ShowWindow(c.rowLbl[r], cmd);
            ShowWindow(c.rowVal[r], cmd);
        }
        for (int k = 0; k < 4; ++k) {
            ShowWindow(c.statLbl[k], cmd);
            ShowWindow(c.statVal[k], cmd);
            ShowWindow(c.cntLbl[k], cmd);
            ShowWindow(c.cntVal[k], cmd);
        }
        for (HWND h : c.sep) ShowWindow(h, cmd);
    }
    ShowWindow(g_rawEdit, show && g_showRaw ? SW_SHOW : SW_HIDE);
    ShowWindow(g_waitLabel, show ? SW_HIDE : SW_SHOW);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

constexpr int RAW_HEIGHT = 190;
constexpr int CARRY_LINES = 5;         // the Carrying list scrolls beyond this
constexpr int SPEED_PART_WIDTH = 400;  // status bar part showing game speed

// Places one party member's controls from the top of its column and sizes the
// group box to fit them; returns that height.
int LayoutChar(CharCtl& c, int gx, int gy, int gw) {
    const int pad = S(10), L = g_lineH;
    const int x = gx + pad, w = gw - 2 * pad;
    int y = gy + L + S(4);

    auto line = [&](HWND h, int height) {
        MoveWindow(h, x, y, w, height, FALSE);
        y += height;
    };
    auto rule = [&](HWND h) {
        MoveWindow(h, x, y + S(5), w, 2, FALSE);
        y += S(12);
    };
    auto row = [&](int r) {
        MoveWindow(c.rowLbl[r], x, y, w / 2, L, FALSE);
        MoveWindow(c.rowVal[r], x + w / 2, y, w - w / 2, L, FALSE);
        y += L;
    };
    auto grid = [&](HWND* labels, HWND* values) {
        const int gap = S(12), half = (w - gap) / 2, valW = S(30);
        for (int k = 0; k < 4; ++k) {
            int cx = x + (k % 2) * (half + gap), cy = y + (k / 2) * L;
            MoveWindow(labels[k], cx, cy, half - valW, L, FALSE);
            MoveWindow(values[k], cx + half - valW, cy, valW, L, FALSE);
        }
        y += 2 * L;
    };

    // The name, and the condition at the right end.
    const int statusW = S(76);
    MoveWindow(c.name, x, y, w - statusW, g_nameH, FALSE);
    MoveWindow(c.status, x + w - statusW, y + g_nameH - L, statusW, L, FALSE);
    y += g_nameH;
    line(c.kind, L);
    y += S(4);
    row(ROW_HP);
    MoveWindow(c.hpBar, x, y + S(2), w, S(14), FALSE);
    y += S(20);
    row(ROW_MP);
    rule(c.sep[0]);
    grid(c.statLbl, c.statVal);
    rule(c.sep[1]);
    row(ROW_EXP);
    row(ROW_FOOD);
    row(ROW_GOLD);
    rule(c.sep[2]);
    line(c.carryLbl, L);

    // Room for a handful of items; the list scrolls when there are more.
    const int listH = CARRY_LINES * L + S(4);
    MoveWindow(c.carryList, x, y, w, listH, FALSE);
    y += listH + S(8);
    grid(c.cntLbl, c.cntVal);

    const int height = y + pad - gy;
    MoveWindow(c.group, gx, gy, gw, height, FALSE);
    return height;
}

void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    SendMessageW(g_statusBar, WM_SIZE, 0, 0);  // the status bar docks itself
    RECT sb;
    GetWindowRect(g_statusBar, &sb);
    const int W = rc.right, H = rc.bottom - (sb.bottom - sb.top);
    const int m = S(10), gap = S(8);

    const int parts[2] = {std::max(W - S(SPEED_PART_WIDTH), S(120)), -1};
    SendMessageW(g_statusBar, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    for (int i = 0; i < 2; ++i)
        SendMessageW(g_statusBar, SB_SETTEXTW, i, reinterpret_cast<LPARAM>(g_statusParts[i].c_str()));

    const int colW = (W - 2 * m - 3 * gap) / 4;
    for (int i = 0; i < 4; ++i) g_columnH = LayoutChar(g_chars[i], m + i * (colW + gap), m, colW);

    // The raw pane takes whatever is left below the columns.
    const int rawY = m + g_columnH + gap;
    if (g_showRaw) MoveWindow(g_rawEdit, m, rawY, W - 2 * m, std::max(H - m - rawY, S(60)), FALSE);
    MoveWindow(g_waitLabel, 0, 0, W, H, FALSE);

    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// Client height that shows the party columns in full, plus the raw pane when open.
int MinClientHeight() {
    RECT sb{};
    if (g_statusBar) GetWindowRect(g_statusBar, &sb);
    const int columnH = g_columnH ? g_columnH : S(600);
    return 2 * S(10) + columnH + (sb.bottom - sb.top) + (g_showRaw ? S(RAW_HEIGHT) + S(8) : 0);
}

SIZE WindowSizeFor(HWND hwnd, int clientW, int clientH) {
    RECT r{0, 0, clientW, clientH};
    AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), GetMenu(hwnd) != nullptr,
                       static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
    return {r.right - r.left, r.bottom - r.top};
}

// ---------------------------------------------------------------------------
// Rendering snapshots
// ---------------------------------------------------------------------------

void SetStatusPart(int part, const std::wstring& text) {
    if (g_statusParts[part] == text) return;
    g_statusParts[part] = text;
    SendMessageW(g_statusBar, SB_SETTEXTW, part, reinterpret_cast<LPARAM>(text.c_str()));
}

void SetBar(CharCtl& c, int max, int pos, int state) {
    if (max == c.barMax && pos == c.barPos && state == c.barState) return;
    c.barMax = max;
    c.barPos = pos;
    c.barState = state;
    // A themed bar ignores new positions while yellow (paused) or red (error)
    // and animates any growth. So go normal, jump straight to the position by
    // overshooting one and stepping back, then apply the colour.
    SendMessageW(c.hpBar, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(c.hpBar, PBM_SETRANGE32, 0, max + 1);
    SendMessageW(c.hpBar, PBM_SETPOS, pos + 1, 0);
    SendMessageW(c.hpBar, PBM_SETPOS, pos, 0);
    SendMessageW(c.hpBar, PBM_SETRANGE32, 0, max);
    SendMessageW(c.hpBar, PBM_SETSTATE, state, 0);
}

void SetCarried(CharCtl& c, std::vector<std::wstring> items) {
    if (items == c.carried) return;
    c.carried = std::move(items);
    SendMessageW(c.carryList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(c.carryList, LB_RESETCONTENT, 0, 0);
    for (const auto& s : c.carried) SendMessageW(c.carryList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    SendMessageW(c.carryList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(c.carryList, nullptr, TRUE);
}

void SetEmptyFlag(CharCtl& c, bool empty) {
    if (c.empty != empty) {
        c.empty = empty;
        InvalidateRect(c.name, nullptr, TRUE);
    }
}

void ShowEmpty(CharCtl& c, int slot) {
    SetEmptyFlag(c, true);
    SetText(c.group, L"");
    SetText(c.name, L"— empty —");
    if (c.levelUp) {
        c.levelUp = false;
        InvalidateRect(c.rowVal[ROW_EXP], nullptr, TRUE);
    }
    SetText(c.kind, L"");
    SetText(c.status, L"");
    SetBar(c, 1, 0, PBST_NORMAL);
    for (HWND h : c.rowVal) SetText(h, L"");
    for (HWND h : c.statVal) SetText(h, L"");
    for (HWND h : c.cntVal) SetText(h, L"");
    c.items.clear();
    SetCarried(c, {});
}

std::wstring MenuEscape(const std::wstring& s);

COLORREF StatusColour(char code) {
    switch (code) {
        case 'G': return COL_GOOD;
        case 'D': return COL_DEAD;
        case 'A': return COL_ASHES;
        default: return COL_POISONED;
    }
}

void Fill(CharCtl& c, const u3::Character& ch, int slot) {
    SetEmptyFlag(c, false);
    // The group box caption carries class, gender and level; group boxes treat '&' as a mnemonic.
    std::wstring caption = ch.klass + L"  ·  " + ch.sex;
    if (ch.level > 0) caption += Format(L"  ·  Level %d", ch.level);
    SetText(c.group, MenuEscape(caption));
    SetText(c.name, ch.name.empty() ? L"(unnamed)" : ch.name);
    SetText(c.kind, ch.race);
    SetText(c.status, ch.status);
    COLORREF colour = StatusColour(ch.statusCode);
    if (colour != c.statusColor) {
        c.statusColor = colour;
        InvalidateRect(c.status, nullptr, TRUE);
    }

    if (ch.hp >= 0 && ch.maxHp > 0) {
        int pos = std::min(ch.hp, ch.maxHp);
        int state = pos * 2 > ch.maxHp ? PBST_NORMAL : (pos * 4 > ch.maxHp ? PBST_PAUSED : PBST_ERROR);
        SetText(c.rowVal[ROW_HP], Format(L"%d / %d", ch.hp, ch.maxHp));
        SetBar(c, ch.maxHp, pos, state);
    } else {
        SetText(c.rowVal[ROW_HP], L"?");
        SetBar(c, 1, 0, PBST_ERROR);
    }

    SetText(c.rowVal[ROW_MP], Num(ch.mp));
    SetText(c.rowVal[ROW_EXP], ch.canLevelUp ? L"▲ " + Num(ch.exp) : Num(ch.exp));
    if (ch.canLevelUp != c.levelUp) {
        c.levelUp = ch.canLevelUp;
        InvalidateRect(c.rowVal[ROW_EXP], nullptr, TRUE);
    }
    SetText(c.rowVal[ROW_FOOD], Num(ch.food));
    SetText(c.rowVal[ROW_GOLD], Num(ch.gold));

    const int stats[4] = {ch.strength, ch.dexterity, ch.intelligence, ch.wisdom};
    const int counters[4] = {ch.gems, ch.keys, ch.powders, ch.torches};
    for (int k = 0; k < 4; ++k) {
        SetText(c.statVal[k], Num(stats[k]));
        SetText(c.cntVal[k], Num(counters[k]));
    }

    std::vector<std::wstring> lines;
    for (const auto& item : ch.carried) lines.push_back(Format(L"%ls  × %d", item.name.c_str(), item.count));
    if (lines.empty()) lines.push_back(L"(nothing)");
    // Equipping changes no text, so repaint when only that changed.
    bool repaint = false;
    for (size_t i = 0; i < ch.carried.size() && i < c.items.size(); ++i)
        repaint = repaint || ch.carried[i].equipped != c.items[i].equipped;
    c.items = ch.carried;
    SetCarried(c, std::move(lines));
    if (repaint) InvalidateRect(c.carryList, nullptr, TRUE);
}

std::wstring HexDump(const uint8_t* d) {
    std::wstring out = L"Party header (18 bytes), then 4 × 64-byte character records\r\n\r\n";
    for (size_t off = 0; off < u3::PARTY_SIZE; off += 16) {
        size_t n = std::min<size_t>(16, u3::PARTY_SIZE - off);
        std::wstring tag = off < u3::HEADER_SIZE
                               ? std::wstring(L"hdr")
                               : Format(L"c%u+%02X", static_cast<unsigned>((off - u3::HEADER_SIZE) / u3::RECORD_SIZE),
                                        static_cast<unsigned>((off - u3::HEADER_SIZE) % u3::RECORD_SIZE));
        std::wstring hex, text;
        for (size_t i = 0; i < 16; ++i) {
            if (i < n) {
                uint8_t b = d[off + i];
                hex += Format(L"%02x ", b);
                text += (b >= 32 && b < 127) ? static_cast<wchar_t>(b) : L'.';
            } else {
                hex += L"   ";
            }
        }
        out += Format(L"%04X  %-7ls  ", static_cast<unsigned>(off), tag.c_str()) + hex + L" " + text + L"\r\n";
    }
    return out;
}

void UpdateRaw() {
    if (!g_showRaw || !g_haveRaw) return;
    std::wstring hex = HexDump(g_lastRaw.data());
    if (hex == g_lastHex) return;
    g_lastHex = hex;
    LRESULT first = SendMessageW(g_rawEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(g_rawEdit, hex.c_str());
    SendMessageW(g_rawEdit, EM_LINESCROLL, 0, first);
}

std::wstring DescribeSpeed(const u3::GameSpeed& speed) {
    if (speed.paused) return L"paused — turns pass only when you act";
    const wchar_t* pace = speed.passSeconds == u3::NORMAL_PASS_SECONDS ? L"normal"
                          : speed.passSeconds < u3::NORMAL_PASS_SECONDS ? L"accelerated"
                                                                        : L"slowed down";
    return Format(L"%ls — an idle turn passes after %d s", pace, speed.passSeconds);
}

void RenderSpeed(const Snapshot& s) {
    std::wstring text = L"Game speed: ";
    if (s.speed.sites == 0) {
        text += DescribeSpeed(u3::GameSpeed{PASS_STEPS[g_step], g_paused}) + L" (waiting for the game)";
    } else {
        text += DescribeSpeed(s.speed.actual);
        if (!s.speed.applied) text += L" — " + s.speed.error;
    }
    SetStatusPart(1, L"\t\t" + text);  // two tabs right-align it
}

// Tells the map windows where the party is.
void UpdateMaps(const Snapshot& s) {
    u3maps::UpdateLocation(s.ok && s.hasLocation ? s.location : u3::Location{}, s.gameFolder);
}

// Hands the reference windows what the Spells "Castable only" filter needs.
void UpdateReferences(const Snapshot& s, const u3::Party* party) {
    u3ref::UpdateParty(u3::ref::PartyStateOf(party, s.combatTurn));
}

// The emulator, for the Debug menu.
std::wstring SourceName(const Snapshot& s) {
    return s.pid ? Format(L"%ls (pid %lu)", s.exe.c_str(), static_cast<unsigned long>(s.pid)) : s.exe;
}

void Render(const Snapshot& s) {
    g_live = s.ok;
    g_sourceCount = s.ok ? s.candidates : 0;
    g_sourceIndex = s.index;
    SetText(g_hwnd, std::wstring(APP_TITLE) + (s.ok ? L" (Connected)" : L" (Not connected)"));
    ShowParty(s.ok);
    RenderSpeed(s);
    UpdateMaps(s);
    if (!s.ok) {
        g_debugInfo = s.exe.empty() ? std::wstring(L"Not connected") : SourceName(s) + L", no party found";
        // Say why once, rather than overwriting later messages on every poll.
        const std::wstring problem = s.error.empty() ? L"Waiting for a party in memory…" : s.error;
        if (problem != g_problem) SetStatusPart(0, g_problem = problem);
        for (int i = 0; i < 4; ++i) ShowEmpty(g_chars[i], i);
        UpdateReferences(s, nullptr);
        return;
    }
    if (!g_problem.empty()) {
        g_problem.clear();
        SetStatusPart(0, L"Connected to " + s.exe + L".");
    }

    u3::Party party = u3::DecodeParty(s.raw.data());
    g_party = party;
    UpdateReferences(s, &party);
    g_debugInfo = SourceName(s) + L" @ " + Hex64(s.address);

    for (int i = 0; i < 4; ++i) {
        const u3::Character& ch = party.chars[i];
        if (i < party.count && ch.present)
            Fill(g_chars[i], ch, i);
        else
            ShowEmpty(g_chars[i], i);
    }

    g_lastRaw = s.raw;
    g_haveRaw = true;
    UpdateRaw();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

u3::GameSpeed WantedSpeed() { return u3::GameSpeed{g_wantSeconds, g_wantPaused}; }

// The game's shops unequip a character's weapon or armour on any sale. Put it
// back when they still own it, by comparing this poll with the last one.
void RestoreSoldEquipment(u3::DosBoxReader& reader, u3::PartyBytes& previous, uint64_t& previousAt,
                          u3::PartyBytes& current) {
    const uint64_t at = reader.Address();
    if (previousAt == at && reader.canWrite) {
        const std::vector<u3::Equip> lost = u3::EquipmentLostToSale(previous.data(), current.data());
        for (const u3::Equip& equip : lost) {
            auto result = std::make_unique<u3::ActionResult>(reader.SetEquipped(equip));
            if (result->ok) result->message = L"Re-equipped after the sale: " + result->message;
            if (PostMessageW(g_hwnd, WM_APP_ACTION, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        }
        if (!lost.empty()) reader.Poll(current);  // show the restored equipment straight away
    }
    previous = current;
    previousAt = at;
}

DWORD WINAPI Worker(LPVOID) {
    u3::DosBoxReader reader(settings::GetString(L"Staging", L"Host", L"127.0.0.1"),
                            settings::GetInt(L"Staging", L"Port", 8086));
    u3::PartyBytes previous{};
    uint64_t previousAt = 0;  // where `previous` was read from; 0 when there's nothing to compare
    std::wstring gameFolder;  // looked up once per emulator
    uint64_t gameFolderSession = 0;
    for (;;) {
        if (g_wantRescan.exchange(false) && reader.Attach()) reader.Scan();
        if (g_wantNext.exchange(false)) reader.NextCandidate();

        if (int action = g_action.exchange(0)) {
            auto result = std::make_unique<u3::ActionResult>();
            u3::PartyBytes current;
            if (!reader.Poll(current))
                result->message = reader.lastError.empty() ? L"No party in memory." : reader.lastError;
            else if (action & ACTION_EQUIP)
                *result = reader.SetEquipped(UnpackEquip(action));
            else if (action & ACTION_MOVE)
                *result = reader.MoveItems(UnpackMove(action));
            else if (action >= ACTION_CURE && action < ACTION_CURE + 4)
                *result = reader.Cure(action - ACTION_CURE);
            else if (action >= ACTION_HEAL && action < ACTION_HEAL + 4)
                *result = reader.FullHealth(action - ACTION_HEAL);
            else if (action >= ACTION_REVIVE && action < ACTION_REVIVE + 4)
                *result = reader.Revive(action - ACTION_REVIVE);
            else if (action == ACTION_FOOD)
                *result = reader.DistributeFood();
            else
                *result = reader.PoolGold(action - ACTION_POOL);
            if (PostMessageW(g_hwnd, WM_APP_ACTION, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        }

        auto snap = std::make_unique<Snapshot>();
        snap->ok = reader.Poll(snap->raw);
        if (snap->ok) {
            RestoreSoldEquipment(reader, previous, previousAt, snap->raw);
            snap->combatTurn = reader.CombatTurn();
            snap->hasLocation = reader.ReadLocation(snap->raw, snap->location);
            if (reader.Session() != gameFolderSession) {
                gameFolderSession = reader.Session();
                gameFolder = reader.GameFolder();
            }
            snap->gameFolder = gameFolder;
            snap->speed = reader.SyncSpeed(WantedSpeed());
        } else {
            previousAt = 0;
        }
        snap->error = reader.lastError;
        snap->exe = reader.exe;
        snap->pid = reader.pid;
        snap->address = reader.Address();
        snap->candidates = reader.candidates.size();
        snap->index = reader.index;

        const bool ok = snap->ok;
        if (PostMessageW(g_hwnd, WM_APP_SNAPSHOT, 0, reinterpret_cast<LPARAM>(snap.get()))) snap.release();

        // Poll briskly while live; back off while there's nothing to find. A
        // button or menu request wakes the loop straight away.
        const HANDLE waits[2] = {g_stopEvent, g_wakeEvent};
        if (WaitForMultipleObjects(2, waits, FALSE, ok ? 250 : 1000) == WAIT_OBJECT_0) break;
    }

    // Don't leave the game paused or slowed once nothing is showing it.
    reader.SyncSpeed(u3::GameSpeed{}, false);
    return 0;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

// The About box: a task dialog whose links open in the browser.
HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

void ShowAbout(HWND owner) {
    const std::wstring title = std::wstring(L"About ") + APP_TITLE;
    const std::wstring heading = std::wstring(APP_TITLE) + L" " + TEXT(APP_VERSION_TEXT);
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = title.c_str();
    dialog.hMainIcon = reinterpret_cast<HICON>(GetClassLongPtrW(owner, GCLP_HICON));
    dialog.pszMainInstruction = heading.c_str();
    dialog.pszContent = L"A live party viewer and editor for Ultima III: Exodus.\n\n"
                        L"Author: Krzysztof Kania\n"
                        L"Website: <a href=\"https://kkania.com\">kkania.com</a>\n"
                        L"Source: <a href=\"https://github.com/kylofon/U3tool\">github.com/kylofon/U3tool</a>\n"
                        L"Support: <a href=\"https://buymeacoffee.com/krzysztofkania\">buymeacoffee.com/krzysztofkania</a>";
    dialog.pfCallback = AboutCallback;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
}

// Remembers where every window is and which are open, for the next run.
void SaveLayout(HWND hwnd) {
    settings::SaveWindow(L"Main", hwnd);
    u3ref::SaveOpenWindows();
    u3maps::SaveOpenWindows();
}

void RequestAction(int action, const wchar_t* pending) {
    SetStatusPart(0, pending);
    g_action = action;
    SetEvent(g_wakeEvent);
}

// Accelerate and Slow down also resume from a pause.
void SetSpeed(int step, bool paused) {
    g_step = std::min(std::max(step, 0), STEP_COUNT - 1);
    g_paused = paused;
    g_wantSeconds = PASS_STEPS[g_step];
    g_wantPaused = paused;
    SetEvent(g_wakeEvent);
}

// Menus treat '&' as a mnemonic marker, so double any in character names.
std::wstring MenuEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t ch : s) {
        if (ch == L'&') out += L'&';
        out += ch;
    }
    return out;
}

// Fills a sub-menu with one item per party member. `describe(character, &info)`
// supplies the text after the tab and returns whether the item applies.
template <typename Describe>
void RebuildMemberMenu(HMENU menu, int baseId, Describe describe) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    if (g_live) {
        for (int i = 0; i < g_party.count && i < 4; ++i) {
            const u3::Character& ch = g_party.chars[i];
            std::wstring info;
            const bool applies = describe(ch, &info);
            AppendMenuW(menu, MF_STRING | (applies ? 0 : MF_GRAYED), baseId + i,
                        (MenuEscape(ch.name) + L"\t" + info).c_str());
        }
    }
    if (GetMenuItemCount(menu) == 0) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(no party)");
}

bool IsDead(const u3::Character& ch) { return ch.statusCode == 'D' || ch.statusCode == 'A'; }

void RebuildPoolMenu() {
    RebuildMemberMenu(g_poolMenu, IDM_POOL_BASE, [](const u3::Character& ch, std::wstring* info) {
        *info = Num(ch.gold) + L" gold";
        return true;
    });
}

void RebuildReviveMenu() {
    RebuildMemberMenu(g_reviveMenu, IDM_REVIVE_BASE, [](const u3::Character& ch, std::wstring* info) {
        *info = ch.status;
        return IsDead(ch);
    });
}

void RebuildHealMenu() {
    RebuildMemberMenu(g_healMenu, IDM_HEAL_BASE, [](const u3::Character& ch, std::wstring* info) {
        if (IsDead(ch)) {
            *info = ch.status;
            return false;
        }
        *info = Format(L"%ls / %ls HP", Num(ch.hp).c_str(), Num(ch.maxHp).c_str());
        return ch.hp >= 0 && ch.maxHp > 0 && ch.hp < ch.maxHp;
    });
}

void RebuildCureMenu() {
    RebuildMemberMenu(g_cureMenu, IDM_CURE_BASE, [](const u3::Character& ch, std::wstring* info) {
        *info = ch.status;
        return ch.statusCode == 'P';
    });
}

bool ColourFor(HWND h, COLORREF* colour) {
    if (h == g_waitLabel) return *colour = GetSysColor(COLOR_GRAYTEXT), true;
    for (const CharCtl& c : g_chars) {
        if (h == c.status) return *colour = c.statusColor, true;
        if (h == c.rowVal[ROW_EXP] && c.levelUp) return *colour = COL_GOOD, true;
        if (h == c.name && c.empty) return *colour = GetSysColor(COLOR_GRAYTEXT), true;
    }
    return false;
}

void ToggleRaw(HWND hwnd) {
    g_showRaw = !g_showRaw;
    ShowWindow(g_rawEdit, g_showRaw && g_partyShown ? SW_SHOW : SW_HIDE);
    g_lastHex.clear();
    UpdateRaw();

    // Grow or shrink the window by the pane's height so the party columns keep their size.
    if (!IsZoomed(hwnd)) {
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int delta = S(RAW_HEIGHT) + S(8);
        SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + (g_showRaw ? delta : -delta),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
}

// ---------------------------------------------------------------------------
// Pop-ups
// ---------------------------------------------------------------------------

// A captioned pop-up with the given client size, centred over the main window.
HWND CreatePopup(const wchar_t* cls, const wchar_t* title, int clientW, int clientH) {
    RECT rc{0, 0, clientW, clientH};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT owner;
    GetWindowRect(g_hwnd, &owner);
    return CreateWindowExW(exStyle, cls, title, style, owner.left + (owner.right - owner.left - w) / 2,
                           owner.top + (owner.bottom - owner.top - h) / 2, w, h, g_hwnd, nullptr, g_inst, nullptr);
}

HWND PopupChild(HWND dlg, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                DWORD exStyle = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    return c;
}

// Shows `dlg` modally until `done`, then destroys it. Snapshots keep arriving
// for the main window meanwhile.
void RunModal(HWND dlg, HWND focus, const bool& done) {
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(focus);

    MSG msg;
    while (!done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);  // before destroying, so activation returns to the main window
    DestroyWindow(dlg);
}

// Preferences --------------------------------------------------------------

struct PrefsDialog {
    HWND topmostCheck = nullptr;
    bool topmost = false;
    bool done = false, accepted = false;
} g_prefs;

void SetTopmost(bool on) {
    g_topmost = on;
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    u3ref::SetTopmost(on);
    u3maps::SetTopmost(on);
    settings::SetInt(L"Preferences", L"AlwaysOnTop", on ? 1 : 0);
}

LRESULT CALLBACK PrefsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                g_prefs.accepted = LOWORD(wp) == IDOK;
                g_prefs.topmost = IsChecked(g_prefs.topmostCheck);
                g_prefs.done = true;
                return 0;
            }
            break;
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_CLOSE:
            g_prefs.done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowPreferences() {
    g_prefs = PrefsDialog{};
    const int pad = S(12), rowH = S(26), btnW = S(80), clientW = S(300);
    const int buttonsY = pad + rowH + S(14);
    HWND dlg = CreatePopup(L"U3StatsPrefs", L"Preferences", clientW, buttonsY + rowH + pad);
    if (!dlg) return;

    g_prefs.topmostCheck = PopupChild(dlg, L"BUTTON", L"Always on &top", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_TOPMOST,
                                      pad, pad, clientW - 2 * pad, rowH);
    SendMessageW(g_prefs.topmostCheck, BM_SETCHECK, g_topmost ? BST_CHECKED : BST_UNCHECKED, 0);
    PopupChild(dlg, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, clientW - pad - 2 * btnW - S(8), buttonsY,
               btnW, rowH);
    PopupChild(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, clientW - pad - btnW, buttonsY, btnW,
               rowH);

    RunModal(dlg, g_prefs.topmostCheck, g_prefs.done);
    if (g_prefs.accepted && g_prefs.topmost != g_topmost) SetTopmost(g_prefs.topmost);
}

// Item count ---------------------------------------------------------------

struct CountPrompt {
    HWND edit = nullptr;
    int max = 1, value = 1;
    bool done = false, accepted = false;
} g_prompt;

void SetPromptValue(int value) {
    g_prompt.value = std::min(std::max(value, 1), g_prompt.max);
    SetText(g_prompt.edit, std::to_wstring(g_prompt.value));
}

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_MINUS:
                    SetPromptValue(g_prompt.value - 1);
                    return 0;
                case IDC_PLUS:
                    SetPromptValue(g_prompt.value + 1);
                    return 0;
                case IDC_COUNT:
                    if (HIWORD(wp) == EN_CHANGE) {
                        // Track what's typed, but only tidy the text once focus leaves.
                        wchar_t buf[8];
                        GetWindowTextW(g_prompt.edit, buf, 8);
                        g_prompt.value = std::min(std::max(_wtoi(buf), 1), g_prompt.max);
                    } else if (HIWORD(wp) == EN_KILLFOCUS) {
                        SetPromptValue(g_prompt.value);
                    }
                    return 0;
                case IDOK:
                    g_prompt.accepted = g_prompt.done = true;
                    return 0;
                case IDCANCEL:
                    g_prompt.done = true;
                    return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            SetPromptValue(g_prompt.value + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1));
            return 0;
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_CLOSE:
            g_prompt.done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Modal pop-up asking for 1..max, starting at max. Returns 0 if cancelled.
int AskCount(const std::wstring& prompt, int max) {
    g_prompt = CountPrompt{};
    g_prompt.max = max;

    const int pad = S(12), rowH = S(26), step = S(28), editW = S(56), btnW = S(80), clientW = S(340);
    const int textH = 2 * g_lineH, countY = pad + textH + S(6), buttonsY = countY + rowH + S(14);
    HWND dlg = CreatePopup(L"U3StatsCount", L"Move items", clientW, buttonsY + rowH + pad);
    if (!dlg) return 0;

    PopupChild(dlg, L"STATIC", prompt.c_str(), SS_NOPREFIX, 0, pad, pad, clientW - 2 * pad, textH);
    int x = pad;
    PopupChild(dlg, L"BUTTON", L"−", BS_PUSHBUTTON | WS_TABSTOP, IDC_MINUS, x, countY, step, rowH);
    x += step + S(4);
    g_prompt.edit = PopupChild(dlg, L"EDIT", L"", ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_COUNT, x,
                               countY, editW, rowH, WS_EX_CLIENTEDGE);
    SendMessageW(g_prompt.edit, EM_SETLIMITTEXT, 2, 0);
    x += editW + S(4);
    PopupChild(dlg, L"BUTTON", L"+", BS_PUSHBUTTON | WS_TABSTOP, IDC_PLUS, x, countY, step, rowH);
    x += step + S(10);
    PopupChild(dlg, L"STATIC", Format(L"of %d", max).c_str(), SS_NOPREFIX | SS_CENTERIMAGE, 0, x, countY,
               clientW - pad - x, rowH);
    PopupChild(dlg, L"BUTTON", L"Move", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, clientW - pad - 2 * btnW - S(8),
               buttonsY, btnW, rowH);
    PopupChild(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, clientW - pad - btnW, buttonsY, btnW,
               rowH);
    SetPromptValue(max);
    SendMessageW(g_prompt.edit, EM_SETSEL, 0, -1);

    RunModal(dlg, g_prompt.edit, g_prompt.done);
    return g_prompt.accepted ? g_prompt.value : 0;
}

// ---------------------------------------------------------------------------
// Dragging items between party members
// ---------------------------------------------------------------------------

int CarryListOwner(HWND list) {
    for (int i = 0; i < 4; ++i)
        if (g_chars[i].carryList == list) return i;
    return -1;
}

// The other live party member whose column is under a screen point, or -1.
int DropTarget(POINT pt) {
    if (!g_live || g_dragFrom < 0) return -1;
    for (int i = 0; i < g_party.count && i < 4; ++i) {
        RECT r;
        GetWindowRect(g_chars[i].group, &r);
        if (i != g_dragFrom && PtInRect(&r, pt)) return i;
    }
    return -1;
}

void GiveItems(int from, int to, const u3::CarriedItem& item) {
    if (!g_live || !g_haveRaw) return;
    u3::ItemMove move;
    move.from = from;
    move.to = to;
    move.armour = item.armour;
    move.type = item.type;

    std::wstring why;
    const int movable = u3::MovableItems(g_lastRaw.data(), move, &why);
    if (movable == 0) {
        MessageBoxW(g_hwnd, why.c_str(), APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }

    // Offer no more than the dragged line shows: the equipped item and its
    // spares are separate lines.
    move.count = std::min(movable, item.count);
    if (item.count > 1) {
        move.count = AskCount(Format(L"Move how many %ls from %ls to %ls?", item.name.c_str(),
                                     g_party.chars[from].name.c_str(), g_party.chars[to].name.c_str()),
                              move.count);
        if (move.count == 0) return;
    }
    RequestAction(PackMove(move), L"Moving items…");
}

LRESULT OnDragList(const DRAGLISTINFO& info) {
    switch (info.uNotification) {
        case DL_BEGINDRAG: {
            const int from = CarryListOwner(info.hWnd);
            const int line = LBItemFromPt(info.hWnd, info.ptCursor, FALSE);
            if (!g_live || g_party.count < 2 || from < 0 || from >= g_party.count || line < 0 ||
                line >= static_cast<int>(g_chars[from].items.size()))
                return FALSE;
            g_dragFrom = from;
            g_dragItem = g_chars[from].items[line];
            SetStatusPart(0, L"Drop on another party member to hand the item over.");
            return TRUE;
        }
        case DL_DRAGGING:
            return DropTarget(info.ptCursor) >= 0 ? DL_MOVECURSOR : DL_STOPCURSOR;
        case DL_DROPPED:
            g_dropTo = DropTarget(info.ptCursor);
            SendMessageW(info.hWnd, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            // Let the list box finish the drag before a pop-up takes over.
            if (g_dropTo >= 0) PostMessageW(g_hwnd, WM_APP_DROP, 0, 0);
            else g_dragFrom = -1;
            return 0;
        case DL_CANCELDRAG:
            g_dragFrom = -1;
            SendMessageW(info.hWnd, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Carried lists: drawing and the equip menu
// ---------------------------------------------------------------------------

// Equipped items are drawn in bold with a grey "readied" / "worn" tag.
void DrawCarriedLine(const DRAWITEMSTRUCT& di) {
    const bool selected = (di.itemState & ODS_SELECTED) != 0;
    FillRect(di.hDC, &di.rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    const int member = CarryListOwner(di.hwndItem);
    if (member < 0 || di.itemID >= g_chars[member].carried.size()) return;

    const CharCtl& c = g_chars[member];
    const u3::CarriedItem* item = di.itemID < c.items.size() ? &c.items[di.itemID] : nullptr;  // else "(nothing)"
    const bool equipped = item && item->equipped;
    RECT r = di.rcItem;
    r.left += S(4);
    r.right -= S(4);
    SetBkMode(di.hDC, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(di.hDC, g_font);

    if (equipped) {
        const wchar_t* tag = item->armour ? L"worn" : L"readied";
        RECT tr = r;
        DrawTextW(di.hDC, tag, -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        SetTextColor(di.hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
        DrawTextW(di.hDC, tag, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        r.right -= (tr.right - tr.left) + S(8);
        SelectObject(di.hDC, g_fontBold);
    }
    SetTextColor(di.hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : item ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
    DrawTextW(di.hDC, c.carried[di.itemID].c_str(), -1, &r,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(di.hDC, oldFont);
    if (di.itemState & ODS_FOCUS) DrawFocusRect(di.hDC, &di.rcItem);
}

// Right-click on a carried item: Ready / Wear it, or put it away.
void ShowItemMenu(int member, LPARAM lp) {
    HWND list = g_chars[member].carryList;
    POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    int line;
    if (pt.x == -1 && pt.y == -1) {  // from the keyboard: use the selected line
        line = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        RECT r{};
        SendMessageW(list, LB_GETITEMRECT, line, reinterpret_cast<LPARAM>(&r));
        pt = {r.left + S(16), r.bottom};
        ClientToScreen(list, &pt);
    } else {
        line = LBItemFromPt(list, pt, FALSE);
    }
    if (!g_live || !g_haveRaw || member >= g_party.count || line < 0 ||
        line >= static_cast<int>(g_chars[member].items.size()))
        return;

    const u3::CarriedItem item = g_chars[member].items[line];
    u3::Equip equip;
    equip.member = member;
    equip.armour = item.armour;
    equip.type = item.equipped ? 0 : item.type;
    const std::wstring label = (item.equipped ? (item.armour ? L"&Take off " : L"&Put away ")
                                              : (item.armour ? L"&Wear " : L"&Ready ")) +
                               item.name;
    std::wstring problem = u3::EquipProblem(g_lastRaw.data(), equip);
    // A spare of the type already in use has nothing left to ready or wear.
    if (!item.equipped)
        for (const auto& other : g_chars[member].items)
            if (other.equipped && other.armour == item.armour && other.type == item.type)
                problem = item.armour ? L"One is already being worn." : L"One is already readied.";

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (problem.empty() ? 0 : MF_GRAYED), IDM_EQUIP, label.c_str());
    if (!problem.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, MenuEscape(problem).c_str());
    SendMessageW(list, LB_SETCURSEL, line, 0);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    DestroyMenu(menu);

    if (cmd == IDM_EQUIP) RequestAction(PackEquip(equip), item.equipped ? L"Unequipping…" : L"Equipping…");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_dragListMsg && msg == g_dragListMsg) return OnDragList(*reinterpret_cast<DRAGLISTINFO*>(lp));

    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            CreateControls();
            ShowParty(false);  // until the first snapshot finds a party
            Layout();
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
            return 0;

        case WM_SIZE:
            Layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            const SIZE min = WindowSizeFor(hwnd, S(900), MinClientHeight());
            mm->ptMinTrackSize.x = min.cx;
            mm->ptMinTrackSize.y = min.cy;
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_QUIT:
                    DestroyWindow(hwnd);
                    return 0;
                case IDM_ABOUT:
                    ShowAbout(hwnd);
                    return 0;
                case IDM_FOOD:
                    RequestAction(ACTION_FOOD, L"Distributing food…");
                    return 0;
                case IDM_FASTER:
                    SetSpeed(g_step - 1, false);
                    return 0;
                case IDM_SLOWER:
                    SetSpeed(g_step + 1, false);
                    return 0;
                case IDM_PAUSE:
                    SetSpeed(g_step, !g_paused);
                    return 0;
                case IDM_NORMAL:
                    SetSpeed(NORMAL_STEP, false);
                    return 0;
                case IDM_PREFERENCES:
                    ShowPreferences();
                    return 0;
                case IDM_RESCAN:
                    SetStatusPart(0, L"Scanning DOSBox memory…");
                    g_wantRescan = true;
                    SetEvent(g_wakeEvent);
                    return 0;
                case IDM_NEXT:
                    g_wantNext = true;
                    SetEvent(g_wakeEvent);
                    return 0;
                case IDM_RAW:
                    ToggleRaw(hwnd);
                    return 0;
            }
            if (LOWORD(wp) >= IDM_MAPS_BASE && LOWORD(wp) < IDM_MAPS_BASE + u3maps::KIND_COUNT) {
                u3maps::Show(static_cast<u3maps::Kind>(LOWORD(wp) - IDM_MAPS_BASE), hwnd, g_font, g_dpi, g_topmost);
                return 0;
            }
            if (LOWORD(wp) >= IDM_REFERENCE_BASE && LOWORD(wp) < IDM_REFERENCE_BASE + u3ref::KIND_COUNT) {
                u3ref::Show(static_cast<u3ref::Kind>(LOWORD(wp) - IDM_REFERENCE_BASE), hwnd, g_font, g_dpi, g_topmost);
                return 0;
            }
            if (LOWORD(wp) >= IDM_REVIVE_BASE && LOWORD(wp) < IDM_REVIVE_BASE + 4) {
                RequestAction(ACTION_REVIVE + (LOWORD(wp) - IDM_REVIVE_BASE), L"Reviving…");
                return 0;
            }
            if (LOWORD(wp) >= IDM_CURE_BASE && LOWORD(wp) < IDM_CURE_BASE + 4) {
                RequestAction(ACTION_CURE + (LOWORD(wp) - IDM_CURE_BASE), L"Curing…");
                return 0;
            }
            if (LOWORD(wp) >= IDM_HEAL_BASE && LOWORD(wp) < IDM_HEAL_BASE + 4) {
                RequestAction(ACTION_HEAL + (LOWORD(wp) - IDM_HEAL_BASE), L"Healing…");
                return 0;
            }
            if (LOWORD(wp) >= IDM_POOL_BASE && LOWORD(wp) < IDM_POOL_BASE + 4) {
                RequestAction(ACTION_POOL + (LOWORD(wp) - IDM_POOL_BASE), L"Pooling gold…");
                return 0;
            }
            break;

        case WM_INITMENUPOPUP:
            if (reinterpret_cast<HMENU>(wp) == g_actionsMenu) {
                // Both actions need a live party of at least two.
                const UINT state = (g_live && g_party.count >= 2) ? MF_ENABLED : MF_GRAYED;
                EnableMenuItem(g_actionsMenu, 0, MF_BYPOSITION | state);
                EnableMenuItem(g_actionsMenu, 1, MF_BYPOSITION | state);
                RebuildPoolMenu();
            } else if (reinterpret_cast<HMENU>(wp) == g_poolMenu) {
                RebuildPoolMenu();  // refresh the gold figures
            } else if (reinterpret_cast<HMENU>(wp) == g_cheatMenu || reinterpret_cast<HMENU>(wp) == g_reviveMenu ||
                       reinterpret_cast<HMENU>(wp) == g_healMenu || reinterpret_cast<HMENU>(wp) == g_cureMenu) {
                RebuildReviveMenu();  // refresh statuses and hit points
                RebuildHealMenu();
                RebuildCureMenu();
            } else if (reinterpret_cast<HMENU>(wp) == g_speedMenu) {
                auto enable = [](int id, bool on) {
                    EnableMenuItem(g_speedMenu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
                };
                enable(IDM_FASTER, g_paused || g_step > 0);
                enable(IDM_SLOWER, g_paused || g_step < STEP_COUNT - 1);
                enable(IDM_NORMAL, g_paused || g_step != NORMAL_STEP);
                CheckMenuItem(g_speedMenu, IDM_PAUSE, MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
            } else if (reinterpret_cast<HMENU>(wp) == g_debugMenu) {
                ModifyMenuW(g_debugMenu, IDM_DEBUG_INFO, MF_BYCOMMAND | MF_STRING | MF_GRAYED, IDM_DEBUG_INFO,
                            MenuEscape(g_debugInfo).c_str());
                // Only worth cycling when the scan found more than one copy.
                const bool several = g_sourceCount > 1;
                const std::wstring next = several ? Format(L"&Next source (%u of %u)",
                                                           static_cast<unsigned>(g_sourceIndex + 1),
                                                           static_cast<unsigned>(g_sourceCount))
                                                  : std::wstring(L"&Next source");
                ModifyMenuW(g_debugMenu, IDM_NEXT, MF_BYCOMMAND | MF_STRING | (several ? MF_ENABLED : MF_GRAYED),
                            IDM_NEXT, next.c_str());
                CheckMenuItem(g_debugMenu, IDM_RAW, MF_BYCOMMAND | (g_showRaw ? MF_CHECKED : MF_UNCHECKED));
            }
            return 0;

        case WM_CONTEXTMENU: {
            const int member = CarryListOwner(reinterpret_cast<HWND>(wp));
            if (member < 0) break;
            ShowItemMenu(member, lp);
            return 0;
        }

        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlType != ODT_LISTBOX) break;
            mi->itemHeight = g_lineH;
            return TRUE;
        }

        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (di->CtlType != ODT_LISTBOX) break;
            DrawCarriedLine(*di);
            return TRUE;
        }

        case WM_APP_DROP: {
            const int from = g_dragFrom;
            g_dragFrom = -1;
            if (from >= 0 && g_dropTo >= 0) GiveItems(from, g_dropTo, g_dragItem);
            return 0;
        }

        case WM_APP_ACTION: {
            std::unique_ptr<u3::ActionResult> result(reinterpret_cast<u3::ActionResult*>(lp));
            SetStatusPart(0, result->message);
            if (!result->ok) MessageBoxW(hwnd, result->message.c_str(), APP_TITLE, MB_OK | MB_ICONWARNING);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND h = reinterpret_cast<HWND>(lp);
            if (h == g_rawEdit) {
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            COLORREF colour;
            if (ColourFor(h, &colour)) {
                SetTextColor(dc, colour);
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        }

        case WM_APP_SNAPSHOT: {
            std::unique_ptr<Snapshot> snap(reinterpret_cast<Snapshot*>(lp));
            Render(*snap);
            return 0;
        }

        case WM_ENDSESSION:
            // Windows is shutting down or signing out: the app is ended without
            // WM_DESTROY, so remember the windows now.
            if (wp) SaveLayout(hwnd);
            return 0;

        case WM_DESTROY:
            SaveLayout(hwnd);
            SetEvent(g_stopEvent);
            WaitForSingleObject(g_thread, 5000);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    crash::Install();
    g_inst = inst;

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    CreateFonts();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"U3StatsWindow";
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);

    WNDCLASSEXW prompt = wc;
    prompt.lpfnWndProc = PromptProc;
    prompt.lpszClassName = L"U3StatsCount";
    RegisterClassExW(&prompt);
    prompt.lpfnWndProc = PrefsProc;
    prompt.lpszClassName = L"U3StatsPrefs";
    RegisterClassExW(&prompt);
    u3ref::Register(inst, wc.hIcon, wc.hIconSm);
    u3maps::Register(inst, wc.hIcon, wc.hIconSm);

    g_dragListMsg = RegisterWindowMessageW(DRAGLISTMSGSTRING);

    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, IDM_PREFERENCES, L"&Preferences…");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_QUIT, L"&Quit\tAlt+F4");

    g_poolMenu = CreatePopupMenu();
    AppendMenuW(g_poolMenu, MF_STRING | MF_GRAYED, 0, L"(no party)");  // filled in when opened
    g_actionsMenu = CreatePopupMenu();
    AppendMenuW(g_actionsMenu, MF_STRING, IDM_FOOD, L"&Distribute food");
    AppendMenuW(g_actionsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_poolMenu), L"&Pool gold");

    HMENU menuBar = CreateMenu();
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_actionsMenu), L"&Actions");

    g_speedMenu = CreatePopupMenu();
    AppendMenuW(g_speedMenu, MF_STRING, IDM_FASTER, L"&Accelerate");
    AppendMenuW(g_speedMenu, MF_STRING, IDM_SLOWER, L"&Slow down");
    AppendMenuW(g_speedMenu, MF_STRING, IDM_PAUSE, L"&Pause");
    AppendMenuW(g_speedMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_speedMenu, MF_STRING, IDM_NORMAL, L"&Normal speed");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_speedMenu), L"Game &speed");

    HMENU referenceMenu = CreatePopupMenu();
    AppendMenuW(referenceMenu, MF_STRING, IDM_REFERENCE_BASE + u3ref::WEAPONS, L"&Weapons");
    AppendMenuW(referenceMenu, MF_STRING, IDM_REFERENCE_BASE + u3ref::ARMOUR, L"&Armour");
    AppendMenuW(referenceMenu, MF_STRING, IDM_REFERENCE_BASE + u3ref::SPELLS, L"&Spells");
    HMENU mapsMenu = CreatePopupMenu();
    AppendMenuW(mapsMenu, MF_STRING, IDM_MAPS_BASE + u3maps::WORLD, L"&World");
    AppendMenuW(mapsMenu, MF_STRING, IDM_MAPS_BASE + u3maps::DUNGEONS, L"&Dungeons");
    AppendMenuW(referenceMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(referenceMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(mapsMenu), L"&Maps");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(referenceMenu), L"&Reference");

    g_reviveMenu = CreatePopupMenu();
    AppendMenuW(g_reviveMenu, MF_STRING | MF_GRAYED, 0, L"(no party)");  // filled in when opened
    g_healMenu = CreatePopupMenu();
    AppendMenuW(g_healMenu, MF_STRING | MF_GRAYED, 0, L"(no party)");
    g_cheatMenu = CreatePopupMenu();
    AppendMenuW(g_cheatMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_reviveMenu), L"&Revive");
    AppendMenuW(g_cheatMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_healMenu), L"&Full health");
    g_cureMenu = CreatePopupMenu();
    AppendMenuW(g_cureMenu, MF_STRING | MF_GRAYED, 0, L"(no party)");
    AppendMenuW(g_cheatMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_cureMenu), L"C&ure");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_cheatMenu), L"&Cheat");

    g_debugMenu = CreatePopupMenu();
    AppendMenuW(g_debugMenu, MF_STRING | MF_GRAYED, IDM_DEBUG_INFO, L"Not connected");  // refreshed when opened
    AppendMenuW(g_debugMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_debugMenu, MF_STRING, IDM_RESCAN, L"&Rescan memory");
    AppendMenuW(g_debugMenu, MF_STRING, IDM_NEXT, L"&Next source");
    AppendMenuW(g_debugMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_debugMenu, MF_STRING, IDM_RAW, L"Raw &bytes");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_debugMenu), L"&Debug");
    AppendMenuW(menuBar, MF_STRING, IDM_ABOUT, L"A&bout");

    g_topmost = settings::GetInt(L"Preferences", L"AlwaysOnTop", 1) != 0;
    const std::wstring title = std::wstring(APP_TITLE) + L" (Not connected)";
    HWND hwnd = CreateWindowExW(g_topmost ? WS_EX_TOPMOST : 0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, S(1080), S(790), nullptr, menuBar, inst, nullptr);
    if (!hwnd) return 1;
    // Fit the height to the party columns.
    const SIZE size = WindowSizeFor(hwnd, S(1080), MinClientHeight());
    SetWindowPos(hwnd, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!settings::RestoreWindow(L"Main", hwnd)) ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    u3ref::RestoreOpenWindows(hwnd, g_font, g_dpi, g_topmost);
    u3maps::RestoreOpenWindows(hwnd, g_font, g_dpi, g_topmost);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
