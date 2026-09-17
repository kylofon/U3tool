// maps.cpp -- World and Dungeons map windows, drawn from the game's own files
// with the tables and decoding in core/mapdata.
#include "maps.h"

#include "mapdata.h"
#include "settings.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

namespace u3maps {
namespace {

constexpr int IDC_MAP_LABEL = 1, IDC_MAP = 2, IDC_LEVEL_LABEL = 3, IDC_LEVEL = 4, IDC_REVEAL = 5, IDC_CLEAR = 6,
              IDC_VIEW = 7, IDC_LEGEND = 8;
constexpr UINT_PTR BLINK_TIMER = 1;
constexpr UINT BLINK_MS = 500;
const wchar_t* const FRAME_CLASS = L"U3AssistantMap";
const wchar_t* const VIEW_CLASS = L"U3AssistantMapView";
const wchar_t* const DEFAULT_FOLDER = L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Ultima 3";

using namespace u3;  // the map tables and helpers

const wchar_t* const TITLES[KIND_COUNT] = {L"World map — Ultima III Assistant", L"Dungeon maps — Ultima III Assistant"};
const wchar_t* const PLACEMENT_KEYS[KIND_COUNT] = {L"MapWorld", L"MapDungeons"};
const wchar_t* const OPEN_KEYS[KIND_COUNT] = {L"WorldOpen", L"DungeonsOpen"};
const wchar_t* const PLACE_KEYS[KIND_COUNT] = {L"World", L"Dungeon"};

struct Window {
    HWND frame = nullptr, view = nullptr;
    int place = 0;              // index into WORLD_PLACES or DUNGEON_PLACES
    int level = 0;              // dungeons only
    bool reveal = false;        // dungeons only
    int followed = -2;          // the party's place (and level) last followed
    bool blinkOn = true;
    std::vector<uint8_t> data;  // the chosen map's file
    HBITMAP image = nullptr;    // world maps, drawn once per choice
};

COLORREF Ref(uint32_t rgb) { return RGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255); }

HINSTANCE g_inst;
HFONT g_font;
int g_dpi = 96;
bool g_appClosing = false;  // once set, windows closing no longer count as closed by the user
// When each window was last closed by the user. Windows' taskbar "Close all
// windows" closes these just before the main window, so a window closed that
// recently still counts as open when the app quits.
ULONGLONG g_closedAt[KIND_COUNT];
constexpr ULONGLONG CLOSE_ALL_GRACE_MS = 2000;
Window g_windows[KIND_COUNT];
Location g_where;
std::wstring g_folder;
Tiles g_tiles;
bool g_tilesLoaded = false;  // tried to load them from g_folder
std::map<std::wstring, ExploredCells> g_progress;
// The dungeon the party is in, for working out what it sees.
int g_partyDungeon = -1;
std::vector<uint8_t> g_partyDungeonData;

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

int PlaceCount(Kind kind) { return kind == WORLD ? WORLD_PLACE_COUNT : DUNGEON_PLACE_COUNT; }
const Place& PlaceAt(Kind kind, int index) { return kind == WORLD ? WORLD_PLACES[index] : DUNGEON_PLACES[index]; }

// Files --------------------------------------------------------------------

HBITMAP WorldBitmap(const std::vector<uint8_t>& map) {
    if (!g_tilesLoaded) {
        g_tiles.Load(g_folder);
        g_tilesLoaded = true;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = WORLD_PIXELS;
    info.bmiHeader.biHeight = -WORLD_PIXELS;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    // A 32-bit DIB pixel is 0x00RRGGBB, just like the rendered ones.
    const std::vector<uint32_t> pixels = RenderWorld(map, g_tiles);
    std::copy(pixels.begin(), pixels.end(), static_cast<uint32_t*>(bits));
    return bitmap;
}

// Where the party is -----------------------------------------------------------

int CurrentWorldPlace() { return WorldPlaceOf(g_where); }
int CurrentDungeon() { return DungeonOf(g_where); }

bool ShowingParty(Kind kind) {
    const Window& w = g_windows[kind];
    return kind == WORLD ? w.place == CurrentWorldPlace()
                         : w.place == CurrentDungeon() && w.level == g_where.level;
}

// Explored dungeon cells, kept in the settings file ------------------------------

ExploredCells& Progress(int dungeon, int level) {
    const std::wstring key = ExploredKey(dungeon, level);
    auto found = g_progress.find(key);
    if (found != g_progress.end()) return found->second;
    return g_progress[key] = ExploredFromHex(settings::GetString(L"Dungeon progress", key.c_str(), L""));
}

void SaveProgress(int dungeon, int level) {
    settings::SetString(L"Dungeon progress", ExploredKey(dungeon, level).c_str(),
                        ExploredToHex(Progress(dungeon, level)));
}

// Marks what the party can see as explored.
void Explore() {
    const int dungeon = CurrentDungeon();
    if (dungeon < 0 || !g_windows[DUNGEONS].frame) return;
    constexpr size_t LEVEL_CELLS = DUNGEON_SIZE * DUNGEON_SIZE;
    if (dungeon != g_partyDungeon) {
        g_partyDungeonData =
            LoadGameFile(g_folder, DUNGEON_PLACES[dungeon].file, false, DUNGEON_LEVELS * LEVEL_CELLS);
        g_partyDungeon = dungeon;
    }
    if (g_partyDungeonData.empty()) return;
    if (ExploreView(Progress(dungeon, g_where.level), &g_partyDungeonData[g_where.level * LEVEL_CELLS], g_where.x,
                    g_where.y, g_where.facing, g_where.torch > 0)) {
        SaveProgress(dungeon, g_where.level);
        if (g_windows[DUNGEONS].view) InvalidateRect(g_windows[DUNGEONS].view, nullptr, FALSE);
    }
}

// Choosing what to show ---------------------------------------------------------

void LoadPlace(Kind kind) {
    Window& w = g_windows[kind];
    if (w.image) DeleteObject(w.image);
    w.image = nullptr;
    const Place& place = PlaceAt(kind, w.place);
    if (kind == WORLD) {
        w.data = LoadGameFile(g_folder, place.file, w.place < SAVED_WORLDS, WORLD_SIZE * WORLD_SIZE);
        if (!w.data.empty()) w.image = WorldBitmap(w.data);
    } else {
        w.data = LoadGameFile(g_folder, place.file, false, DUNGEON_LEVELS * DUNGEON_SIZE * DUNGEON_SIZE);
    }
    if (w.view) InvalidateRect(w.view, nullptr, FALSE);
}

void SelectPlace(Kind kind, int place) {
    Window& w = g_windows[kind];
    w.place = std::max(0, std::min(place, PlaceCount(kind) - 1));
    if (w.frame) SendDlgItemMessageW(w.frame, IDC_MAP, CB_SETCURSEL, w.place, 0);
    settings::SetInt(L"Maps", PLACE_KEYS[kind], w.place);
    LoadPlace(kind);
}

void SelectLevel(int level) {
    Window& w = g_windows[DUNGEONS];
    w.level = std::max(0, std::min(level, DUNGEON_LEVELS - 1));
    if (w.frame) SendDlgItemMessageW(w.frame, IDC_LEVEL, CB_SETCURSEL, w.level, 0);
    settings::SetInt(L"Maps", L"DungeonLevel", w.level);
    if (w.view) InvalidateRect(w.view, nullptr, FALSE);
}

// Shows the party's map when the party gets to a new one, but leaves alone a map
// the user picked while it stays put.
void FollowParty() {
    Window& world = g_windows[WORLD];
    if (world.frame) {
        const int place = CurrentWorldPlace();
        if (place != world.followed) {
            world.followed = place;
            if (place >= 0 && place != world.place) SelectPlace(WORLD, place);
        }
    }
    Window& dungeons = g_windows[DUNGEONS];
    if (dungeons.frame) {
        const int dungeon = CurrentDungeon();
        const int key = dungeon < 0 ? -1 : dungeon * DUNGEON_LEVELS + g_where.level;
        if (key != dungeons.followed) {
            dungeons.followed = key;
            if (dungeon >= 0) {
                if (dungeon != dungeons.place) SelectPlace(DUNGEONS, dungeon);
                SelectLevel(g_where.level);
            }
        }
        Explore();
    }
}

void ClearProgress() {
    Window& w = g_windows[DUNGEONS];
    const std::wstring question = L"Forget the explored parts of " + std::wstring(DUNGEON_PLACES[w.place].name) +
                                  L", on all eight levels?";
    if (MessageBoxW(w.frame, question.c_str(), L"Clear map", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    for (int level = 0; level < DUNGEON_LEVELS; ++level) {
        Progress(w.place, level).fill(0);
        SaveProgress(w.place, level);
    }
    InvalidateRect(w.view, nullptr, FALSE);
}

// Painting ----------------------------------------------------------------------

void FillSolid(HDC dc, const RECT& r, COLORREF colour) {
    SetDCBrushColor(dc, colour);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Frame(HDC dc, RECT r, COLORREF colour, int thickness) {
    for (int i = 0; i < thickness; ++i) {
        SetDCBrushColor(dc, colour);
        FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        InflateRect(&r, -1, -1);
    }
}

void PaintMessage(HDC dc, const RECT& rc, const std::wstring& text) {
    RECT r = rc;
    InflateRect(&r, -S(16), -S(16));
    HGDIOBJ old = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(dc, text.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

void PaintWorld(HDC dc, const RECT& rc) {
    const Window& w = g_windows[WORLD];
    if (!w.image) {
        PaintMessage(dc, rc, L"Couldn't read " + std::wstring(WORLD_PLACES[w.place].file) + L" in " + g_folder +
                                 L". Start the game with the assistant running so it can find the game's folder.");
        return;
    }
    const int side = std::max(0L, std::min(rc.right, rc.bottom));
    const int ox = (rc.right - side) / 2, oy = (rc.bottom - side) / 2;
    const int px = WORLD_PIXELS;

    HDC source = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(source, w.image);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, ox, oy, side, side, source, 0, 0, px, px, SRCCOPY);
    SelectObject(source, old);
    DeleteDC(source);

    if (w.blinkOn && ShowingParty(WORLD)) {
        const double cell = side / double(WORLD_SIZE);
        RECT r{ox + int(g_where.x * cell), oy + int(g_where.y * cell), ox + int((g_where.x + 1) * cell + 0.999),
               oy + int((g_where.y + 1) * cell + 0.999)};
        InflateRect(&r, S(3), S(3));
        Frame(dc, r, Ref(PARTY_BOX_EDGE), S(2));
        InflateRect(&r, -S(2), -S(2));
        Frame(dc, r, Ref(PARTY_BOX), S(2));
    }
}

void PaintArrow(HDC dc, int cx, int cy, int cell, int facing) {
    const int r = std::max(cell * 4 / 10, 3);
    const POINT north[3] = {{0, -r}, {r, r}, {-r, r}};
    POINT points[3];
    for (int i = 0; i < 3; ++i) {
        int x = north[i].x, y = north[i].y;
        for (int turn = 0; turn < facing; ++turn) {  // a quarter turn clockwise each
            const int nx = -y;
            y = x;
            x = nx;
        }
        points[i] = {cx + x, cy + y};
    }
    HBRUSH fill = CreateSolidBrush(Ref(PARTY_ARROW));
    HPEN pen = CreatePen(PS_SOLID, std::max(S(2), 1), Ref(PARTY_ARROW_EDGE));
    HGDIOBJ oldBrush = SelectObject(dc, fill), oldPen = SelectObject(dc, pen);
    Polygon(dc, points, 3);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);
}

void PaintDungeon(HDC dc, const RECT& rc) {
    Window& w = g_windows[DUNGEONS];
    if (w.data.size() < size_t(DUNGEON_LEVELS * DUNGEON_SIZE * DUNGEON_SIZE)) {
        PaintMessage(dc, rc, L"Couldn't read " + std::wstring(DUNGEON_PLACES[w.place].file) + L" in " + g_folder +
                                 L". Start the game with the assistant running so it can find the game's folder.");
        return;
    }
    const int cell = std::max(static_cast<int>(std::min(rc.right, rc.bottom)) / DUNGEON_SIZE, 1);
    const int grid = cell * DUNGEON_SIZE;
    const int ox = (rc.right - grid) / 2, oy = (rc.bottom - grid) / 2;
    const ExploredCells& explored = Progress(w.place, w.level);

    HFONT font = CreateFontW(-std::max(cell * 6 / 10, 6), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    for (int y = 0; y < DUNGEON_SIZE; ++y) {
        for (int x = 0; x < DUNGEON_SIZE; ++x) {
            const RECT r{ox + x * cell, oy + y * cell, ox + (x + 1) * cell, oy + (y + 1) * cell};
            const uint8_t value = w.data[w.level * DUNGEON_SIZE * DUNGEON_SIZE + y * DUNGEON_SIZE + x];
            const CellLook look = DungeonCell(value, w.reveal || IsExplored(explored, x, y));
            FillSolid(dc, r, Ref(look.fill));
            if (look.mark) {
                RECT text = r;
                SetTextColor(dc, Ref(look.ink));
                DrawTextW(dc, look.mark, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    }
    // Grid lines.
    SetDCPenColor(dc, Ref(DUNGEON_GRID));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    for (int i = 0; i <= DUNGEON_SIZE; ++i) {
        MoveToEx(dc, ox + i * cell, oy, nullptr);
        LineTo(dc, ox + i * cell, oy + grid);
        MoveToEx(dc, ox, oy + i * cell, nullptr);
        LineTo(dc, ox + grid, oy + i * cell);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldFont);
    DeleteObject(font);

    if (w.blinkOn && ShowingParty(DUNGEONS)) {
        const int x = g_where.x % DUNGEON_SIZE, y = g_where.y % DUNGEON_SIZE;
        PaintArrow(dc, ox + x * cell + cell / 2, oy + y * cell + cell / 2, cell, g_where.facing);
    }
}

// Windows -------------------------------------------------------------------------

int KindOf(HWND frame) {
    for (int k = 0; k < KIND_COUNT; ++k)
        if (g_windows[k].frame == frame) return k;
    return -1;
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // Draw off-screen first so the blinking cursor doesn't flicker the map.
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(buffer, bitmap);
            FillRect(buffer, &rc, GetSysColorBrush(COLOR_BTNFACE));
            if (GetWindowLongPtrW(hwnd, GWLP_USERDATA) == DUNGEONS)
                PaintDungeon(buffer, rc);
            else
                PaintWorld(buffer, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int TopRowHeight() { return S(8) * 2 + S(24); }
int LegendHeight(Kind kind) { return kind == DUNGEONS ? 3 * LineHeight() + S(4) : 0; }

void LayoutFrame(Kind kind) {
    const Window& w = g_windows[kind];
    RECT rc;
    GetClientRect(w.frame, &rc);
    const int pad = S(8), rowH = S(24);
    int x = pad;
    auto place = [&](int id, int width, int extraHeight = 0) {
        if (HWND h = GetDlgItem(w.frame, id)) MoveWindow(h, x, pad, width, rowH + extraHeight, TRUE);
        x += width + S(6);
    };
    place(IDC_MAP_LABEL, kind == WORLD ? S(34) : S(62));
    place(IDC_MAP, kind == WORLD ? S(210) : S(180), S(300));  // a drop-down list's height includes its list
    if (kind == DUNGEONS) {
        place(IDC_LEVEL_LABEL, S(40));
        place(IDC_LEVEL, S(84), S(200));
        place(IDC_REVEAL, S(70));
        place(IDC_CLEAR, S(64));
    }
    const int top = TopRowHeight(), legendH = LegendHeight(kind);
    MoveWindow(w.view, 0, top, rc.right, std::max<int>(rc.bottom - top - legendH, 0), TRUE);
    if (HWND legend = GetDlgItem(w.frame, IDC_LEGEND))
        MoveWindow(legend, pad, rc.bottom - legendH + S(2), std::max<int>(rc.right - 2 * pad, 0), legendH - S(4), TRUE);
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int k = KindOf(hwnd);
    switch (msg) {
        case WM_SIZE:
            if (k >= 0) LayoutFrame(static_cast<Kind>(k));
            return 0;
        case WM_COMMAND:
            if (k < 0) break;
            switch (LOWORD(wp)) {
                case IDC_MAP:
                    if (HIWORD(wp) == CBN_SELCHANGE)
                        SelectPlace(static_cast<Kind>(k),
                                    static_cast<int>(SendDlgItemMessageW(hwnd, IDC_MAP, CB_GETCURSEL, 0, 0)));
                    return 0;
                case IDC_LEVEL:
                    if (HIWORD(wp) == CBN_SELCHANGE)
                        SelectLevel(static_cast<int>(SendDlgItemMessageW(hwnd, IDC_LEVEL, CB_GETCURSEL, 0, 0)));
                    return 0;
                case IDC_REVEAL:
                    g_windows[k].reveal = IsDlgButtonChecked(hwnd, IDC_REVEAL) == BST_CHECKED;
                    settings::SetInt(L"Maps", L"Reveal", g_windows[k].reveal ? 1 : 0);
                    InvalidateRect(g_windows[k].view, nullptr, FALSE);
                    return 0;
                case IDC_CLEAR:
                    ClearProgress();
                    return 0;
            }
            break;
        case WM_TIMER:
            if (wp == BLINK_TIMER && k >= 0) {
                g_windows[k].blinkOn = !g_windows[k].blinkOn;
                if (ShowingParty(static_cast<Kind>(k))) InvalidateRect(g_windows[k].view, nullptr, FALSE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = S(320);
            mm->ptMinTrackSize.y = S(260);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, BLINK_TIMER);
            if (k >= 0) {
                if (!g_appClosing) {
                    settings::SaveWindow(PLACEMENT_KEYS[k], hwnd);
                    settings::SetInt(L"Maps", OPEN_KEYS[k], 0);
                    g_closedAt[k] = GetTickCount64();
                }
                if (g_windows[k].image) DeleteObject(g_windows[k].image);
                g_windows[k] = Window{};
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
    g_folder = settings::GetString(L"Game", L"Folder", DEFAULT_FOLDER);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = FRAME_CLASS;
    wc.hIcon = icon;
    wc.hIconSm = smallIcon;
    RegisterClassExW(&wc);

    WNDCLASSEXW view{};
    view.cbSize = sizeof view;
    view.style = CS_HREDRAW | CS_VREDRAW;
    view.lpfnWndProc = ViewProc;
    view.hInstance = inst;
    view.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    view.lpszClassName = VIEW_CLASS;
    RegisterClassExW(&view);
}

void Show(Kind kind, HWND owner, HFONT font, int dpi, bool topmost) {
    g_font = font;
    g_dpi = dpi;
    Window& w = g_windows[kind];
    if (w.frame) {
        if (IsIconic(w.frame)) ShowWindow(w.frame, SW_RESTORE);
        SetForegroundWindow(w.frame);
        return;
    }

    const int side = kind == WORLD ? S(560) : S(448);
    const int clientW = kind == WORLD ? side : std::max(side, S(560));
    const DWORD style = WS_OVERLAPPEDWINDOW, exStyle = topmost ? WS_EX_TOPMOST : 0;
    RECT r{0, 0, clientW, TopRowHeight() + side + LegendHeight(kind)};
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    const int width = r.right - r.left, height = r.bottom - r.top;

    // Cascade from the assistant's top-left corner, kept on its monitor.
    RECT anchor;
    GetWindowRect(owner, &anchor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    const int step = S(60) * (kind + 1) + S(40);
    const int x = std::max<int>(work.left, std::min<int>(anchor.left + step, work.right - width));
    const int y = std::max<int>(work.top, std::min<int>(anchor.top + step, work.bottom - height));

    w.frame = CreateWindowExW(exStyle, FRAME_CLASS, TITLES[kind], style, x, y, width, height, nullptr, nullptr, g_inst,
                              nullptr);
    if (!w.frame) return;
    // Recorded now, so it's reopened next time even if the app never gets to close normally.
    settings::SetInt(L"Maps", OPEN_KEYS[kind], 1);
    g_closedAt[kind] = 0;

    ChildControl(w.frame, L"STATIC", kind == WORLD ? L"Map:" : L"Dungeon:", SS_CENTERIMAGE | SS_NOPREFIX,
                 IDC_MAP_LABEL);
    HWND places = ChildControl(w.frame, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_MAP);
    for (int i = 0; i < PlaceCount(kind); ++i)
        SendMessageW(places, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(PlaceAt(kind, i).name));
    if (kind == DUNGEONS) {
        ChildControl(w.frame, L"STATIC", L"Level:", SS_CENTERIMAGE | SS_NOPREFIX, IDC_LEVEL_LABEL);
        HWND levels = ChildControl(w.frame, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_LEVEL);
        for (int level = 1; level <= DUNGEON_LEVELS; ++level)
            SendMessageW(levels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((L"Level " + std::to_wstring(level)).c_str()));
        ChildControl(w.frame, L"BUTTON", L"&Reveal", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_REVEAL);
        ChildControl(w.frame, L"BUTTON", L"&Clear", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLEAR);
        ChildControl(w.frame, L"STATIC", DUNGEON_LEGEND, SS_LEFT | SS_NOPREFIX, IDC_LEGEND);
        w.reveal = settings::GetInt(L"Maps", L"Reveal", 0) != 0;
        CheckDlgButton(w.frame, IDC_REVEAL, w.reveal ? BST_CHECKED : BST_UNCHECKED);
    }
    w.view = CreateWindowExW(0, VIEW_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, w.frame,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VIEW)), g_inst, nullptr);
    SetWindowLongPtrW(w.view, GWLP_USERDATA, kind);

    // Open on the party's map when it can be told; otherwise on the last one shown.
    const int current = kind == WORLD ? CurrentWorldPlace() : CurrentDungeon();
    SelectPlace(kind, current >= 0 ? current : settings::GetInt(L"Maps", PLACE_KEYS[kind], 0));
    if (kind == DUNGEONS) SelectLevel(current >= 0 ? g_where.level : settings::GetInt(L"Maps", L"DungeonLevel", 0));
    w.followed = kind == WORLD ? current : (current < 0 ? -1 : current * DUNGEON_LEVELS + g_where.level);
    Explore();

    LayoutFrame(kind);
    SetTimer(w.frame, BLINK_TIMER, BLINK_MS, nullptr);
    if (!settings::RestoreWindow(PLACEMENT_KEYS[kind], w.frame)) ShowWindow(w.frame, SW_SHOWNORMAL);
}

void SetTopmost(bool topmost) {
    for (const Window& w : g_windows)
        if (w.frame)
            SetWindowPos(w.frame, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void UpdateLocation(const Location& where, const std::wstring& gameFolder) {
    if (!gameFolder.empty() && gameFolder != g_folder) {
        g_folder = gameFolder;
        settings::SetString(L"Game", L"Folder", gameFolder);
        g_tiles = Tiles{};
        g_tilesLoaded = false;
        g_partyDungeon = -1;  // reload it from the new folder
        for (int k = 0; k < KIND_COUNT; ++k)
            if (g_windows[k].frame) LoadPlace(static_cast<Kind>(k));
    }

    const bool moved = where.live != g_where.live || where.map != g_where.map || where.entryX != g_where.entryX ||
                       where.entryY != g_where.entryY || where.x != g_where.x || where.y != g_where.y ||
                       where.level != g_where.level || where.facing != g_where.facing;
    g_where = where;
    FollowParty();
    if (moved) {
        for (Window& w : g_windows) {
            if (!w.frame) continue;
            w.blinkOn = true;  // show the cursor at its new spot straight away
            InvalidateRect(w.view, nullptr, FALSE);
        }
    }
}

void RestoreOpenWindows(HWND owner, HFONT font, int dpi, bool topmost) {
    for (int k = 0; k < KIND_COUNT; ++k)
        if (settings::GetInt(L"Maps", OPEN_KEYS[k], 0)) Show(static_cast<Kind>(k), owner, font, dpi, topmost);
}

void SaveOpenWindows() {
    g_appClosing = true;
    const ULONGLONG now = GetTickCount64();
    for (int k = 0; k < KIND_COUNT; ++k) {
        if (g_windows[k].frame) settings::SaveWindow(PLACEMENT_KEYS[k], g_windows[k].frame);
        const bool justClosed = g_closedAt[k] && now - g_closedAt[k] < CLOSE_ALL_GRACE_MS;
        settings::SetInt(L"Maps", OPEN_KEYS[k], g_windows[k].frame || justClosed ? 1 : 0);
    }
}

}  // namespace u3maps
