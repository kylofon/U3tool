// mapframe.cpp -- the map windows: a place (and level) chooser, the map, and
// for dungeons a legend. A blinking cursor marks the party.
#include "mapframe.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

#include "icon.h"
#include "mapdata.h"
#include "settings.h"

namespace mapwin {
namespace {

using namespace u3;  // the map tables and helpers
using Clock = std::chrono::steady_clock;

constexpr int BLINK_MS = 500;
// Where the GOG release usually sits, until the running emulator says otherwise.
wxString DefaultFolder() {
#ifdef __WXMSW__
    return "C:\\Program Files (x86)\\GOG Galaxy\\Games\\Ultima 3";
#else
    return wxGetHomeDir() + "/GOG Games/Ultima 3";
#endif
}

const char* const TITLES[KIND_COUNT] = {"World map", "Dungeon maps"};
const char* const PLACEMENT_KEYS[KIND_COUNT] = {"MapWorld", "MapDungeons"};
const char* const OPEN_KEYS[KIND_COUNT] = {"WorldOpen", "DungeonsOpen"};
const char* const PLACE_KEYS[KIND_COUNT] = {"World", "Dungeon"};
constexpr auto CLOSE_ALL_GRACE = std::chrono::seconds(2);  // see referenceframe.cpp
constexpr size_t LEVEL_CELLS = DUNGEON_SIZE * DUNGEON_SIZE;

wxColour Colour(uint32_t rgb) { return wxColour(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255); }

class MapFrame;
MapFrame* g_frames[KIND_COUNT];
Clock::time_point g_closedAt[KIND_COUNT];
bool g_closedRecently[KIND_COUNT];
bool g_appClosing = false;
Location g_where;
wxString g_folder;
Tiles g_tiles;
bool g_tilesLoaded = false;  // tried to load them from g_folder
std::map<std::wstring, ExploredCells> g_progress;
int g_partyDungeon = -1;  // the dungeon the party is in, for working out what it sees
std::vector<uint8_t> g_partyDungeonData;

int PlaceCount(Kind kind) { return kind == WORLD ? WORLD_PLACE_COUNT : DUNGEON_PLACE_COUNT; }
const Place& PlaceAt(Kind kind, int index) { return kind == WORLD ? WORLD_PLACES[index] : DUNGEON_PLACES[index]; }
int CurrentWorldPlace() { return WorldPlaceOf(g_where); }
int CurrentDungeon() { return DungeonOf(g_where); }

// Explored dungeon cells, kept in the settings file.
ExploredCells& Progress(int dungeon, int level) {
    const std::wstring key = ExploredKey(dungeon, level);
    auto found = g_progress.find(key);
    if (found != g_progress.end()) return found->second;
    return g_progress[key] =
               ExploredFromHex(settings::GetString("Dungeon progress", key, "").ToStdWstring());
}

void SaveProgress(int dungeon, int level) {
    settings::SetString("Dungeon progress", ExploredKey(dungeon, level), ExploredToHex(Progress(dungeon, level)));
}

// Draws `text` word-wrapped and centred in `rect`.
void DrawWrapped(wxDC& dc, const wxString& text, const wxRect& rect) {
    std::vector<wxString> lines;
    wxString line;
    for (const wxString& word : wxSplit(text, ' ', '\0')) {
        const wxString candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && dc.GetTextExtent(candidate).x > rect.width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
    }
    if (!line.empty()) lines.push_back(line);
    int y = rect.y;
    for (const wxString& l : lines) {
        const wxSize size = dc.GetTextExtent(l);
        dc.DrawText(l, rect.x + (rect.width - size.x) / 2, y);
        y += size.y;
    }
}

class MapFrame : public wxFrame {
public:
    MapFrame(Kind kind, wxWindow* owner, bool topmost);
    ~MapFrame() override { g_frames[kind_] = nullptr; }

    void LoadPlace();
    void SelectPlace(int place);
    void SelectLevel(int level);
    void FollowParty();
    void Explore();
    void Moved();

private:
    bool ShowingParty() const;
    void Paint(wxDC& dc, const wxSize& size);
    void PaintWorld(wxDC& dc, const wxSize& size);
    void PaintDungeon(wxDC& dc, const wxSize& size);
    void PaintMessage(wxDC& dc, const wxSize& size, const wxString& text);
    void OnClose(wxCloseEvent& event);
    void ClearProgress();

    Kind kind_;
    wxChoice* places_ = nullptr;
    wxChoice* levels_ = nullptr;
    wxCheckBox* reveal_ = nullptr;
    wxStaticText* legend_ = nullptr;
    wxWindow* view_ = nullptr;
    wxTimer blink_;
    bool blinkOn_ = true;

    int place_ = 0;             // index into WORLD_PLACES or DUNGEON_PLACES
    int level_ = 0;             // dungeons only
    int followed_ = -2;         // the party's place (and level) last followed
    std::vector<uint8_t> data_;  // the chosen map's file
    wxImage world_;             // world maps, drawn once per choice
    wxBitmap scaled_;           // world_ at the view's size
};

MapFrame::MapFrame(Kind kind, wxWindow* owner, bool topmost)
    : wxFrame(nullptr, wxID_ANY, wxString::FromUTF8(TITLES[kind]) + wxString::FromUTF8(" — Ultima III Assistant"),
              wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE | (topmost ? wxSTAY_ON_TOP : 0)),
      kind_(kind),
      blink_(this) {
    SetIcons(AppIcons());
    auto* panel = new wxPanel(this);
    auto* all = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(8);
    const int lineH = GetCharHeight() + FromDIP(4);

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    top->Add(new wxStaticText(panel, wxID_ANY, kind == WORLD ? "Map:" : "Dungeon:"), 0, wxALIGN_CENTER_VERTICAL);
    places_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(kind == WORLD ? 210 : 180), -1));
    for (int i = 0; i < PlaceCount(kind); ++i) places_->Append(wxString(PlaceAt(kind, i).name));
    top->Add(places_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
    if (kind == DUNGEONS) {
        top->Add(new wxStaticText(panel, wxID_ANY, "Level:"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        levels_ = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(84), -1));
        for (int level = 1; level <= DUNGEON_LEVELS; ++level) levels_->Append(wxString::Format("Level %d", level));
        top->Add(levels_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        reveal_ = new wxCheckBox(panel, wxID_ANY, "&Reveal");
        reveal_->SetValue(settings::GetInt("Maps", "Reveal", 0) != 0);
        top->Add(reveal_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        auto* clear = new wxButton(panel, wxID_ANY, "&Clear");
        top->Add(clear, 0, wxALIGN_CENTER_VERTICAL);
        levels_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SelectLevel(levels_->GetSelection()); });
        reveal_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            settings::SetInt("Maps", "Reveal", reveal_->GetValue() ? 1 : 0);
            view_->Refresh(false);
        });
        clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ClearProgress(); });
    }
    all->Add(top, 0, wxALL, pad);
    places_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SelectPlace(places_->GetSelection()); });

    view_ = new wxWindow(panel, wxID_ANY);
    view_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    view_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(view_);  // drawn off-screen so the blinking cursor doesn't flicker
        Paint(dc, view_->GetClientSize());
    });
    view_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        view_->Refresh(false);
        event.Skip();
    });
    all->Add(view_, 1, wxEXPAND);

    int legendH = 0;
    if (kind == DUNGEONS) {
        legendH = 3 * lineH + FromDIP(4);
        legend_ = new wxStaticText(panel, wxID_ANY, wxString(DUNGEON_LEGEND));
        legend_->SetMinSize(wxSize(-1, legendH - FromDIP(4)));
        all->Add(legend_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
#ifndef __WXMSW__
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            event.Skip();
            CallAfter([this] {
                legend_->SetLabelText(wxString(DUNGEON_LEGEND));
                legend_->Wrap(legend_->GetSize().x);
            });
        });
#endif
    }
    panel->SetSizer(all);

    // Open on the party's map when it can be told; otherwise on the last one shown.
    const int current = kind == WORLD ? CurrentWorldPlace() : CurrentDungeon();
    SelectPlace(current >= 0 ? current : settings::GetInt("Maps", PLACE_KEYS[kind], 0));
    if (kind == DUNGEONS) SelectLevel(current >= 0 ? g_where.level : settings::GetInt("Maps", "DungeonLevel", 0));
    followed_ = kind == WORLD ? current : (current < 0 ? -1 : current * DUNGEON_LEVELS + g_where.level);

    const int side = FromDIP(kind == WORLD ? 560 : 448);
    const int clientW = kind == WORLD ? side : std::max(side, FromDIP(560));
    const int topH = top->GetMinSize().y + 2 * pad;
    SetClientSize(clientW, topH + side + legendH);
    SetMinSize(FromDIP(wxSize(320, 260)));

    // Cascade from the assistant's top-left corner, kept on its monitor.
    const int display = wxDisplay::GetFromWindow(owner);
    const wxRect work = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea();
    const wxPoint anchor = owner->GetPosition();
    const wxSize size = GetSize();
    const int step = FromDIP(60) * (kind + 1) + FromDIP(40);
    SetPosition(wxPoint(std::max(work.x, std::min(anchor.x + step, work.GetRight() + 1 - size.x)),
                        std::max(work.y, std::min(anchor.y + step, work.GetBottom() + 1 - size.y))));

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        blinkOn_ = !blinkOn_;
        if (ShowingParty()) view_->Refresh(false);
    });
    blink_.Start(BLINK_MS);
    Bind(wxEVT_CLOSE_WINDOW, &MapFrame::OnClose, this);
}

bool MapFrame::ShowingParty() const {
    return kind_ == WORLD ? place_ == CurrentWorldPlace() : place_ == CurrentDungeon() && level_ == g_where.level;
}

void MapFrame::LoadPlace() {
    world_ = wxImage();
    scaled_ = wxBitmap();
    const Place& place = PlaceAt(kind_, place_);
    const std::wstring folder = g_folder.ToStdWstring();
    if (kind_ == WORLD) {
        data_ = LoadGameFile(folder, place.file, place_ < SAVED_WORLDS, WORLD_SIZE * WORLD_SIZE);
        if (!data_.empty()) {
            if (!g_tilesLoaded) {
                g_tiles.Load(folder);
                g_tilesLoaded = true;
            }
            const std::vector<uint32_t> pixels = RenderWorld(data_, g_tiles);
            world_.Create(WORLD_PIXELS, WORLD_PIXELS, false);
            unsigned char* rgb = world_.GetData();
            for (uint32_t pixel : pixels) {
                *rgb++ = pixel >> 16 & 255;
                *rgb++ = pixel >> 8 & 255;
                *rgb++ = pixel & 255;
            }
        }
    } else {
        data_ = LoadGameFile(folder, place.file, false, DUNGEON_LEVELS * LEVEL_CELLS);
    }
    view_->Refresh(false);
}

void MapFrame::SelectPlace(int place) {
    place_ = std::max(0, std::min(place, PlaceCount(kind_) - 1));
    places_->SetSelection(place_);
    settings::SetInt("Maps", PLACE_KEYS[kind_], place_);
    LoadPlace();
}

void MapFrame::SelectLevel(int level) {
    level_ = std::max(0, std::min(level, DUNGEON_LEVELS - 1));
    if (levels_) levels_->SetSelection(level_);
    settings::SetInt("Maps", "DungeonLevel", level_);
    view_->Refresh(false);
}

// Shows the party's map when the party gets to a new one, but leaves alone a map
// the user picked while it stays put.
void MapFrame::FollowParty() {
    if (kind_ == WORLD) {
        const int place = CurrentWorldPlace();
        if (place != followed_) {
            followed_ = place;
            if (place >= 0 && place != place_) SelectPlace(place);
        }
        return;
    }
    const int dungeon = CurrentDungeon();
    const int key = dungeon < 0 ? -1 : dungeon * DUNGEON_LEVELS + g_where.level;
    if (key != followed_) {
        followed_ = key;
        if (dungeon >= 0) {
            if (dungeon != place_) SelectPlace(dungeon);
            SelectLevel(g_where.level);
        }
    }
    Explore();
}

// Marks what the party can see as explored.
void MapFrame::Explore() {
    const int dungeon = CurrentDungeon();
    if (kind_ != DUNGEONS || dungeon < 0) return;
    if (dungeon != g_partyDungeon) {
        g_partyDungeonData =
            LoadGameFile(g_folder.ToStdWstring(), DUNGEON_PLACES[dungeon].file, false, DUNGEON_LEVELS * LEVEL_CELLS);
        g_partyDungeon = dungeon;
    }
    if (g_partyDungeonData.empty()) return;
    if (ExploreView(Progress(dungeon, g_where.level), &g_partyDungeonData[g_where.level * LEVEL_CELLS], g_where.x,
                    g_where.y, g_where.facing, g_where.torch > 0)) {
        SaveProgress(dungeon, g_where.level);
        view_->Refresh(false);
    }
}

void MapFrame::Moved() {
    blinkOn_ = true;  // show the cursor at its new spot straight away
    view_->Refresh(false);
}

void MapFrame::ClearProgress() {
    const wxString question = wxString("Forget the explored parts of ") + wxString(DUNGEON_PLACES[place_].name) +
                              ", on all eight levels?";
    if (wxMessageBox(question, "Clear map", wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
    for (int level = 0; level < DUNGEON_LEVELS; ++level) {
        Progress(place_, level).fill(0);
        SaveProgress(place_, level);
    }
    view_->Refresh(false);
}

void MapFrame::Paint(wxDC& dc, const wxSize& size) {
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
    dc.Clear();
    dc.SetFont(GetFont());
    if (kind_ == WORLD)
        PaintWorld(dc, size);
    else
        PaintDungeon(dc, size);
}

void MapFrame::PaintMessage(wxDC& dc, const wxSize& size, const wxString& text) {
    dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    const int margin = FromDIP(16);
    DrawWrapped(dc, text, wxRect(margin, margin, size.x - 2 * margin, size.y - 2 * margin));
}

void MapFrame::PaintWorld(wxDC& dc, const wxSize& size) {
    if (!world_.IsOk()) {
        PaintMessage(dc, size, wxString("Couldn't read ") + wxString(WORLD_PLACES[place_].file) + " in " + g_folder +
                                   ". Start the game with the assistant running so it can find the game's folder.");
        return;
    }
    const int side = std::max(0, std::min(size.x, size.y));
    if (side == 0) return;
    const int ox = (size.x - side) / 2, oy = (size.y - side) / 2;
    if (!scaled_.IsOk() || scaled_.GetWidth() != side) scaled_ = wxBitmap(world_.Scale(side, side, wxIMAGE_QUALITY_HIGH));
    dc.DrawBitmap(scaled_, ox, oy);

    if (blinkOn_ && ShowingParty()) {
        const double cell = side / double(WORLD_SIZE);
        wxRect r(wxPoint(ox + int(g_where.x * cell), oy + int(g_where.y * cell)),
                 wxPoint(ox + int((g_where.x + 1) * cell + 0.999) - 1, oy + int((g_where.y + 1) * cell + 0.999) - 1));
        r.Inflate(FromDIP(3));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        const int thick = FromDIP(2);
        for (int ring = 0; ring < 2 * thick; ++ring) {
            dc.SetPen(wxPen(Colour(ring < thick ? PARTY_BOX_EDGE : PARTY_BOX), 1));
            dc.DrawRectangle(r);
            r.Deflate(1);
        }
    }
}

void MapFrame::PaintDungeon(wxDC& dc, const wxSize& size) {
    if (data_.size() < DUNGEON_LEVELS * LEVEL_CELLS) {
        PaintMessage(dc, size, wxString("Couldn't read ") + wxString(DUNGEON_PLACES[place_].file) + " in " + g_folder +
                                   ". Start the game with the assistant running so it can find the game's folder.");
        return;
    }
    const int cell = std::max(std::min(size.x, size.y) / DUNGEON_SIZE, 1);
    const int grid = cell * DUNGEON_SIZE;
    const int ox = (size.x - grid) / 2, oy = (size.y - grid) / 2;
    const ExploredCells& explored = Progress(place_, level_);
    const bool reveal = reveal_ && reveal_->GetValue();

    dc.SetFont(wxFont(wxFontInfo(wxSize(0, std::max(cell * 6 / 10, 6))).Bold()));
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (int y = 0; y < DUNGEON_SIZE; ++y) {
        for (int x = 0; x < DUNGEON_SIZE; ++x) {
            const wxRect r(ox + x * cell, oy + y * cell, cell, cell);
            const uint8_t value = data_[level_ * LEVEL_CELLS + y * DUNGEON_SIZE + x];
            const CellLook look = DungeonCell(value, reveal || IsExplored(explored, x, y));
            dc.SetBrush(wxBrush(Colour(look.fill)));
            dc.DrawRectangle(r);
            if (look.mark) {
                const wxString mark(look.mark);
                const wxSize extent = dc.GetTextExtent(mark);
                dc.SetTextForeground(Colour(look.ink));
                dc.DrawText(mark, r.x + (r.width - extent.x) / 2, r.y + (r.height - extent.y) / 2);
            }
        }
    }
    // Grid lines.
    dc.SetPen(wxPen(Colour(DUNGEON_GRID), 1));
    for (int i = 0; i <= DUNGEON_SIZE; ++i) {
        dc.DrawLine(ox + i * cell, oy, ox + i * cell, oy + grid);
        dc.DrawLine(ox, oy + i * cell, ox + grid, oy + i * cell);
    }

    if (blinkOn_ && ShowingParty()) {
        // A triangle pointing the way the party faces, turned a quarter at a time.
        const int x = g_where.x % DUNGEON_SIZE, y = g_where.y % DUNGEON_SIZE;
        const int cx = ox + x * cell + cell / 2, cy = oy + y * cell + cell / 2;
        const int r = std::max(cell * 4 / 10, 3);
        const wxPoint north[3] = {{0, -r}, {r, r}, {-r, r}};
        wxPoint points[3];
        for (int i = 0; i < 3; ++i) {
            int px = north[i].x, py = north[i].y;
            for (int turn = 0; turn < g_where.facing; ++turn) {
                const int nx = -py;
                py = px;
                px = nx;
            }
            points[i] = wxPoint(cx + px, cy + py);
        }
        dc.SetBrush(wxBrush(Colour(PARTY_ARROW)));
        dc.SetPen(wxPen(Colour(PARTY_ARROW_EDGE), std::max(FromDIP(2), 1)));
        dc.DrawPolygon(3, points);
    }
}

void MapFrame::OnClose(wxCloseEvent& event) {
    blink_.Stop();
    if (!g_appClosing) {
        settings::SaveWindow(PLACEMENT_KEYS[kind_], this);
        settings::SetInt("Maps", OPEN_KEYS[kind_], 0);
        g_closedAt[kind_] = Clock::now();
        g_closedRecently[kind_] = true;
    }
    event.Skip();  // destroys the window
}

}  // namespace

void Show(Kind kind, wxWindow* owner, bool topmost) {
    if (g_folder.empty()) g_folder = settings::GetString("Game", "Folder", DefaultFolder());
    if (MapFrame* open = g_frames[kind]) {
        if (open->IsIconized()) open->Iconize(false);
        open->Raise();
        return;
    }
    auto* frame = new MapFrame(kind, owner, topmost);
    g_frames[kind] = frame;
    // Recorded now, so it's reopened next time even if the app never gets to close normally.
    settings::SetInt("Maps", OPEN_KEYS[kind], 1);
    g_closedRecently[kind] = false;
    frame->Explore();
    if (!settings::RestoreWindow(PLACEMENT_KEYS[kind], frame)) frame->Show();
}

void SetTopmost(bool topmost) {
    for (MapFrame* frame : g_frames) {
        if (!frame) continue;
        const long style = frame->GetWindowStyleFlag();
        frame->SetWindowStyleFlag(topmost ? style | wxSTAY_ON_TOP : style & ~wxSTAY_ON_TOP);
    }
}

void UpdateLocation(const Location& where, const wxString& gameFolder) {
    if (g_folder.empty()) g_folder = settings::GetString("Game", "Folder", DefaultFolder());
    if (!gameFolder.empty() && gameFolder != g_folder) {
        g_folder = gameFolder;
        settings::SetString("Game", "Folder", gameFolder);
        g_tiles = Tiles{};
        g_tilesLoaded = false;
        g_partyDungeon = -1;  // reload it from the new folder
        for (MapFrame* frame : g_frames)
            if (frame) frame->LoadPlace();
    }

    const bool moved = where.live != g_where.live || where.map != g_where.map || where.entryX != g_where.entryX ||
                       where.entryY != g_where.entryY || where.x != g_where.x || where.y != g_where.y ||
                       where.level != g_where.level || where.facing != g_where.facing;
    g_where = where;
    for (MapFrame* frame : g_frames) {
        if (!frame) continue;
        frame->FollowParty();
        if (moved) frame->Moved();
    }
}

void RestoreOpenWindows(wxWindow* owner, bool topmost) {
    if (g_folder.empty()) g_folder = settings::GetString("Game", "Folder", DefaultFolder());
    for (int kind = 0; kind < KIND_COUNT; ++kind)
        if (settings::GetInt("Maps", OPEN_KEYS[kind], 0)) Show(static_cast<Kind>(kind), owner, topmost);
}

void SaveAndCloseAll() {
    g_appClosing = true;
    const auto now = Clock::now();
    for (int kind = 0; kind < KIND_COUNT; ++kind) {
        MapFrame* frame = g_frames[kind];
        if (frame) settings::SaveWindow(PLACEMENT_KEYS[kind], frame);
        const bool justClosed = g_closedRecently[kind] && now - g_closedAt[kind] < CLOSE_ALL_GRACE;
        settings::SetInt("Maps", OPEN_KEYS[kind], frame || justClosed ? 1 : 0);
        if (frame) frame->Destroy();
    }
}

}  // namespace mapwin
