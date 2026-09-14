// main.cpp -- U3Stats: a native Win32 live party viewer for Ultima III.
//
// A worker thread polls DOSBox through u3::DosBoxReader and posts snapshots
// to the window; the UI thread decodes them and updates standard controls,
// touching only what actually changed so nothing flickers.
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

#include "reader.h"

namespace {

constexpr UINT WM_APP_SNAPSHOT = WM_APP + 1;
constexpr UINT WM_APP_ACTION = WM_APP + 2;

enum : int { IDC_RESCAN = 100, IDC_NEXT, IDC_TOPMOST, IDC_RAW, IDC_RAWEDIT };
enum : int {
    IDM_QUIT = 200,
    IDM_FOOD,
    IDM_FASTER,
    IDM_SLOWER,
    IDM_PAUSE,
    IDM_NORMAL,
    IDM_POOL_BASE = 210,  // IDM_POOL_BASE + party member
};

// Requests handed to the worker thread; pooling adds the member index.
constexpr int ACTION_FOOD = 1;
constexpr int ACTION_POOL = 10;

enum Row { ROW_HP, ROW_MP, ROW_EXP, ROW_FOOD, ROW_GOLD, ROW_WEAPON, ROW_ARMOUR, ROW_COUNT };
const wchar_t* const ROW_LABELS[ROW_COUNT] = {L"Hit points", L"Magic points", L"Experience", L"Food",
                                              L"Gold",       L"Weapon",       L"Armour"};
const wchar_t* const STAT_LABELS[4] = {L"Strength", L"Dexterity", L"Intelligence", L"Wisdom"};
const wchar_t* const COUNTER_LABELS[4] = {L"Gems", L"Keys", L"Powders", L"Torches"};

const COLORREF COL_GOOD = RGB(0, 128, 0);
const COLORREF COL_POISONED = RGB(170, 110, 0);
const COLORREF COL_DEAD = RGB(192, 0, 0);
const COLORREF COL_ASHES = RGB(110, 110, 110);
const COLORREF COL_SPEED_CHANGED = RGB(170, 110, 0);
const COLORREF COL_SPEED_PAUSED = RGB(0, 90, 180);

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
};

struct CharCtl {
    HWND group, name, kind, status, hpBar, carryLbl, carryList;
    HWND rowLbl[ROW_COUNT], rowVal[ROW_COUNT];
    HWND statLbl[4], statVal[4];
    HWND cntLbl[4], cntVal[4];
    HWND sep[4];
    COLORREF statusColor = COL_GOOD;
    bool empty = true;
    int barMax = -1, barPos = -1, barState = -1;
    std::vector<std::wstring> carried;
};

HINSTANCE g_inst;
HWND g_hwnd, g_statusText, g_speedText, g_rescan, g_next, g_topmost, g_rawCheck, g_rawEdit, g_statusBar;
HMENU g_actionsMenu, g_poolMenu, g_speedMenu;
CharCtl g_chars[4];
COLORREF g_statusColor = CLR_INVALID, g_speedColor = CLR_INVALID;
HFONT g_font, g_fontBold, g_fontName, g_fontMono;
int g_dpi = 96, g_lineH = 18, g_nameH = 24;
bool g_showRaw = false;
bool g_haveRaw = false;
u3::PartyBytes g_lastRaw{};
std::wstring g_lastHex;

bool g_live = false;
u3::Party g_party;  // last decoded party, for building the Pool gold menu

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
    g_statusText = Label(L"Looking for DOSBox…", g_font, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    g_speedText = Label(L"", g_fontBold, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    g_rescan = Child(L"BUTTON", L"&Rescan", BS_PUSHBUTTON | WS_TABSTOP, g_font, IDC_RESCAN);
    g_next = Child(L"BUTTON", L"&Next source", BS_PUSHBUTTON | WS_TABSTOP, g_font, IDC_NEXT);
    g_topmost = Child(L"BUTTON", L"Always on &top", BS_AUTOCHECKBOX | WS_TABSTOP, g_font, IDC_TOPMOST);
    g_rawCheck = Child(L"BUTTON", L"Raw &bytes", BS_AUTOCHECKBOX | WS_TABSTOP, g_font, IDC_RAW);
    SendMessageW(g_topmost, BM_SETCHECK, BST_CHECKED, 0);

    for (int i = 0; i < 4; ++i) {
        CharCtl& c = g_chars[i];
        c.name = Label(L"— empty —", g_fontName, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
        c.kind = Label(L"", g_font, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
        c.status = Label(L"", g_fontBold);
        c.hpBar = Child(PROGRESS_CLASSW, L"", 0, g_font);
        for (int r = 0; r < ROW_COUNT; ++r) {
            c.rowLbl[r] = Label(ROW_LABELS[r], g_font);
            c.rowVal[r] = Label(L"", g_fontBold, SS_RIGHT);
        }
        for (int k = 0; k < 4; ++k) {
            c.statLbl[k] = Label(STAT_LABELS[k], g_font);
            c.statVal[k] = Label(L"", g_fontBold, SS_RIGHT);
            c.cntLbl[k] = Label(COUNTER_LABELS[k], g_font);
            c.cntVal[k] = Label(L"", g_fontBold, SS_RIGHT);
            c.sep[k] = Child(L"STATIC", L"", SS_ETCHEDHORZ, g_font);
        }
        c.carryLbl = Label(L"Carrying", g_font);
        c.carryList = Child(L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_VSCROLL, g_font, 0,
                            WS_EX_CLIENTEDGE);

        // The group box goes to the bottom of the z-order and clips its
        // siblings, so it never paints over the controls it frames.
        c.group = Child(L"BUTTON", Format(L"Party member %d", i + 1).c_str(), BS_GROUPBOX | WS_CLIPSIBLINGS,
                        g_font);
        SetWindowPos(c.group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    g_rawEdit = Child(L"EDIT", L"",
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                      g_fontMono, IDC_RAWEDIT, WS_EX_CLIENTEDGE);
    ShowWindow(g_rawEdit, SW_HIDE);

    g_statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready.", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                  0, g_hwnd, nullptr, g_inst, nullptr);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

constexpr int RAW_HEIGHT = 190;

void LayoutChar(CharCtl& c, int gx, int gy, int gw, int gh) {
    MoveWindow(c.group, gx, gy, gw, gh, FALSE);

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

    line(c.name, g_nameH);
    line(c.kind, L);
    line(c.status, L);
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
    row(ROW_WEAPON);
    row(ROW_ARMOUR);
    rule(c.sep[3]);
    line(c.carryLbl, L);

    // The carried list soaks up whatever height is left above the counters.
    int countersY = gy + gh - pad - 2 * L;
    int listH = std::max(countersY - S(8) - y, S(40));
    MoveWindow(c.carryList, x, y, w, listH, FALSE);
    y = std::max(countersY, y + listH + S(8));
    grid(c.cntLbl, c.cntVal);
}

void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    SendMessageW(g_statusBar, WM_SIZE, 0, 0);  // the status bar docks itself
    RECT sb;
    GetWindowRect(g_statusBar, &sb);
    const int W = rc.right, H = rc.bottom - (sb.bottom - sb.top);
    const int m = S(10), gap = S(8), btnH = S(26);

    int x = W - m;
    auto placeRight = [&](HWND h, int width) {
        x -= width;
        MoveWindow(h, x, m, width, btnH, FALSE);
        x -= S(6);
    };
    placeRight(g_rawCheck, S(92));
    placeRight(g_topmost, S(118));
    placeRight(g_next, S(96));
    placeRight(g_rescan, S(80));
    MoveWindow(g_statusText, m, m + (btnH - g_lineH) / 2, std::max(0, x - m), g_lineH, FALSE);

    const int speedY = m + btnH + S(4);
    MoveWindow(g_speedText, m, speedY, W - 2 * m, g_lineH, FALSE);

    const int rawH = g_showRaw ? S(RAW_HEIGHT) : 0;
    const int top = speedY + g_lineH + gap;
    const int bottom = H - m - (g_showRaw ? rawH + gap : 0);
    if (g_showRaw) MoveWindow(g_rawEdit, m, H - m - rawH, W - 2 * m, rawH, FALSE);

    const int colW = (W - 2 * m - 3 * gap) / 4;
    const int groupH = std::max(bottom - top, S(200));
    for (int i = 0; i < 4; ++i) LayoutChar(g_chars[i], m + i * (colW + gap), top, colW, groupH);

    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// Rendering snapshots
// ---------------------------------------------------------------------------

void SetStatus(const std::wstring& text, COLORREF colour) {
    SetText(g_statusText, text);
    if (colour != g_statusColor) {
        g_statusColor = colour;
        InvalidateRect(g_statusText, nullptr, TRUE);
    }
}

void SetBar(CharCtl& c, int max, int pos, int state) {
    if (max != c.barMax) SendMessageW(c.hpBar, PBM_SETRANGE32, 0, c.barMax = max);
    if (state != c.barState) SendMessageW(c.hpBar, PBM_SETSTATE, c.barState = state, 0);
    if (pos != c.barPos) SendMessageW(c.hpBar, PBM_SETPOS, c.barPos = pos, 0);
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
    SetText(c.group, Format(L"Party member %d", slot + 1));
    SetText(c.name, L"— empty —");
    SetText(c.kind, L"");
    SetText(c.status, L"");
    SetBar(c, 1, 0, PBST_NORMAL);
    for (HWND h : c.rowVal) SetText(h, L"");
    for (HWND h : c.statVal) SetText(h, L"");
    for (HWND h : c.cntVal) SetText(h, L"");
    SetCarried(c, {});
}

COLORREF StatusColour(char code) {
    switch (code) {
        case 'G': return COL_GOOD;
        case 'D': return COL_DEAD;
        case 'A': return COL_ASHES;
        default: return COL_POISONED;
    }
}

void Fill(CharCtl& c, const u3::Character& ch, int slot, int rosterSlot) {
    SetEmptyFlag(c, false);
    SetText(c.group, Format(L"Party member %d  ·  roster %d", slot + 1, rosterSlot));
    SetText(c.name, ch.name.empty() ? L"(unnamed)" : ch.name);
    SetText(c.kind, ch.race + L" " + ch.klass + L"  ·  " + ch.sex);
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
    SetText(c.rowVal[ROW_EXP], Num(ch.exp));
    SetText(c.rowVal[ROW_FOOD], Num(ch.food));
    SetText(c.rowVal[ROW_GOLD], Num(ch.gold));
    SetText(c.rowVal[ROW_WEAPON], ch.weapon);
    SetText(c.rowVal[ROW_ARMOUR], ch.armour);

    const int stats[4] = {ch.strength, ch.dexterity, ch.intelligence, ch.wisdom};
    const int counters[4] = {ch.gems, ch.keys, ch.powders, ch.torches};
    for (int k = 0; k < 4; ++k) {
        SetText(c.statVal[k], Num(stats[k]));
        SetText(c.cntVal[k], Num(counters[k]));
    }

    std::vector<std::wstring> items;
    for (const auto& item : ch.carried) items.push_back(Format(L"%ls  × %d", item.first.c_str(), item.second));
    if (items.empty()) items.push_back(L"(nothing)");
    SetCarried(c, std::move(items));
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
    COLORREF colour;
    if (s.speed.sites == 0) {
        text += DescribeSpeed(u3::GameSpeed{PASS_STEPS[g_step], g_paused}) + L"  (waiting for the game)";
        colour = GetSysColor(COLOR_GRAYTEXT);
    } else {
        const u3::GameSpeed& actual = s.speed.actual;
        text += DescribeSpeed(actual);
        if (!s.speed.applied) text += L"  — " + s.speed.error;
        colour = actual.paused                                     ? COL_SPEED_PAUSED
                 : actual.passSeconds != u3::NORMAL_PASS_SECONDS ? COL_SPEED_CHANGED
                                                                   : GetSysColor(COLOR_WINDOWTEXT);
    }
    SetText(g_speedText, text);
    if (colour != g_speedColor) {
        g_speedColor = colour;
        InvalidateRect(g_speedText, nullptr, TRUE);
    }
}

void Render(const Snapshot& s) {
    g_live = s.ok;
    RenderSpeed(s);
    if (!s.ok) {
        SetStatus(s.error.empty() ? L"Waiting for a party in memory…" : s.error, COL_POISONED);
        for (int i = 0; i < 4; ++i) ShowEmpty(g_chars[i], i);
        return;
    }

    u3::Party party = u3::DecodeParty(s.raw.data());
    g_party = party;
    std::wstring status = Format(L"Live  —  %d in party  —  %ls (pid %lu) @ ", party.count, s.exe.c_str(),
                                 static_cast<unsigned long>(s.pid)) +
                          Hex64(s.address);
    if (s.candidates > 1)
        status += Format(L"   [source %u of %u]", static_cast<unsigned>(s.index + 1),
                         static_cast<unsigned>(s.candidates));
    SetStatus(status, COL_GOOD);

    for (int i = 0; i < 4; ++i) {
        const u3::Character& ch = party.chars[i];
        if (i < party.count && ch.present)
            Fill(g_chars[i], ch, i, party.slots[i]);
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

DWORD WINAPI Worker(LPVOID) {
    u3::DosBoxReader reader;
    for (;;) {
        if (g_wantRescan.exchange(false) && reader.Attach()) reader.Scan();
        if (g_wantNext.exchange(false)) reader.NextCandidate();

        if (int action = g_action.exchange(0)) {
            auto result = std::make_unique<u3::ActionResult>();
            u3::PartyBytes current;
            if (!reader.Poll(current))
                result->message = reader.lastError.empty() ? L"No party in memory." : reader.lastError;
            else if (action == ACTION_FOOD)
                *result = reader.DistributeFood();
            else
                *result = reader.PoolGold(action - ACTION_POOL);
            if (PostMessageW(g_hwnd, WM_APP_ACTION, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        }

        auto snap = std::make_unique<Snapshot>();
        snap->ok = reader.Poll(snap->raw);
        if (snap->ok) snap->speed = reader.SyncSpeed(WantedSpeed());
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

void RequestAction(int action, const wchar_t* pending) {
    SendMessageW(g_statusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(pending));
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

void RebuildPoolMenu() {
    while (GetMenuItemCount(g_poolMenu) > 0) DeleteMenu(g_poolMenu, 0, MF_BYPOSITION);
    if (g_live) {
        for (int i = 0; i < g_party.count && i < 4; ++i) {
            const u3::Character& ch = g_party.chars[i];
            AppendMenuW(g_poolMenu, MF_STRING, IDM_POOL_BASE + i,
                        (MenuEscape(ch.name) + L"\t" + Num(ch.gold) + L" gold").c_str());
        }
    }
    if (GetMenuItemCount(g_poolMenu) == 0) AppendMenuW(g_poolMenu, MF_STRING | MF_GRAYED, 0, L"(no party)");
}

bool ColourFor(HWND h, COLORREF* colour) {
    if (h == g_statusText && g_statusColor != CLR_INVALID) return *colour = g_statusColor, true;
    if (h == g_speedText && g_speedColor != CLR_INVALID) return *colour = g_speedColor, true;
    for (const CharCtl& c : g_chars) {
        if (h == c.status) return *colour = c.statusColor, true;
        if (h == c.name && c.empty) return *colour = GetSysColor(COLOR_GRAYTEXT), true;
    }
    return false;
}

void ToggleRaw(HWND hwnd) {
    g_showRaw = IsChecked(g_rawCheck);
    ShowWindow(g_rawEdit, g_showRaw ? SW_SHOW : SW_HIDE);
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            CreateControls();
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
            mm->ptMinTrackSize.x = S(900);
            mm->ptMinTrackSize.y = S(724) + g_lineH + (g_showRaw ? S(RAW_HEIGHT) + S(8) : 0);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_QUIT:
                    DestroyWindow(hwnd);
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
                case IDC_RESCAN:
                    SetStatus(L"Scanning DOSBox memory…", GetSysColor(COLOR_WINDOWTEXT));
                    g_wantRescan = true;
                    SetEvent(g_wakeEvent);
                    return 0;
                case IDC_NEXT:
                    g_wantNext = true;
                    SetEvent(g_wakeEvent);
                    return 0;
                case IDC_TOPMOST:
                    SetWindowPos(hwnd, IsChecked(g_topmost) ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    return 0;
                case IDC_RAW:
                    ToggleRaw(hwnd);
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
            } else if (reinterpret_cast<HMENU>(wp) == g_speedMenu) {
                auto enable = [](int id, bool on) {
                    EnableMenuItem(g_speedMenu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
                };
                enable(IDM_FASTER, g_paused || g_step > 0);
                enable(IDM_SLOWER, g_paused || g_step < STEP_COUNT - 1);
                enable(IDM_NORMAL, g_paused || g_step != NORMAL_STEP);
                CheckMenuItem(g_speedMenu, IDM_PAUSE, MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
            }
            return 0;

        case WM_APP_ACTION: {
            std::unique_ptr<u3::ActionResult> result(reinterpret_cast<u3::ActionResult*>(lp));
            SendMessageW(g_statusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(result->message.c_str()));
            if (!result->ok)
                MessageBoxW(hwnd, result->message.c_str(), L"Ultima III — Party Stats", MB_OK | MB_ICONWARNING);
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

        case WM_DESTROY:
            SetEvent(g_stopEvent);
            WaitForSingleObject(g_thread, 5000);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
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

    HMENU fileMenu = CreatePopupMenu();
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

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Ultima III — Party Stats",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1080), S(790), nullptr,
                                menuBar, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
