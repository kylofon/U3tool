// referenceframe.cpp -- the reference windows: a table, a note underneath and,
// for spells, a "Castable only" filter. Hovering a row tells which party
// members can use it; the Classes header explains the class letters.
#include "referenceframe.h"

#include <wx/checkbox.h>
#include <wx/display.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tooltip.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#endif

#include <algorithm>
#include <chrono>
#include <vector>

#include "dialogs.h"
#include "icon.h"
#include "settings.h"

namespace refwin {
namespace {

using u3::ref::KIND_COUNT;
using u3::ref::SPELLS;
using u3::ref::TABLES;
using u3::ref::Table;
using Clock = std::chrono::steady_clock;

// Settings keys: window placement, and whether the window was open at exit.
const char* const PLACEMENT_KEYS[KIND_COUNT] = {"Weapons", "Armour", "Spells"};
const char* const OPEN_KEYS[KIND_COUNT] = {"WeaponsOpen", "ArmourOpen", "SpellsOpen"};

// Windows' taskbar "Close all windows" closes these just before the main
// window, so a window closed this recently still counts as open at exit.
constexpr auto CLOSE_ALL_GRACE = std::chrono::seconds(2);

class ReferenceFrame;
ReferenceFrame* g_frames[KIND_COUNT];
Clock::time_point g_closedAt[KIND_COUNT];
bool g_closedRecently[KIND_COUNT];
bool g_appClosing = false;
u3::ref::PartyState g_party;

class ReferenceFrame : public wxFrame {
public:
    ReferenceFrame(Kind kind, wxWindow* owner, bool topmost);
    ~ReferenceFrame() override { g_frames[kind_] = nullptr; }

    void Refilter(bool force);

private:
    void FillRows(const std::vector<bool>* visible);
    void OnMotion(wxMouseEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnSize(wxSizeEvent& event);
#ifdef __WXMSW__
    void CreateHeaderTip();
    void PlaceHeaderTip();
    HWND headerTip_ = nullptr;
#endif

    Kind kind_;
    const Table& table_;
    wxListCtrl* list_ = nullptr;
    wxStaticText* note_ = nullptr;
    wxCheckBox* castable_ = nullptr;
    wxStaticText* castableFor_ = nullptr;
    std::vector<bool> shownRows_;  // spells shown by the filter; empty when not filtering
    int tipRow_ = -2;              // the row the tooltip describes; -1 for none
};

ReferenceFrame::ReferenceFrame(Kind kind, wxWindow* owner, bool topmost)
    : wxFrame(nullptr, wxID_ANY, wxString(TABLES[kind].title), wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_FRAME_STYLE | (topmost ? wxSTAY_ON_TOP : 0)),
      kind_(kind),
      table_(TABLES[kind]) {
    SetIcons(AppIcons());
    auto* panel = new wxPanel(this);
    auto* all = new wxBoxSizer(wxVERTICAL);
    const int lineH = GetCharHeight() + FromDIP(4);

    int filterH = 0;
    if (kind == SPELLS) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        castable_ = new wxCheckBox(panel, wxID_ANY, "&Castable only");
        castable_->SetMinSize(wxSize(FromDIP(120), -1));
        castableFor_ = new wxStaticText(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                        wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        row->Add(castable_, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(castableFor_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        filterH = lineH + FromDIP(10);
        all->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        all->SetItemMinSize(row, -1, filterH);
        castable_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Refilter(true); });
    }

    list_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES | wxBORDER_NONE);
    int clientW = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this) + FromDIP(6);
    for (int c = 0; c < table_.columnCount; ++c) {
        const u3::ref::Column& column = table_.columns[c];
        list_->AppendColumn(wxString(column.title),
                            column.align == u3::ref::Align::Right ? wxLIST_FORMAT_RIGHT : wxLIST_FORMAT_LEFT,
                            FromDIP(column.width));
        clientW += FromDIP(column.width);
    }
#ifdef __WXMSW__
    if (table_.Grouped()) {
        const HWND hwnd = static_cast<HWND>(list_->GetHWND());
        ListView_EnableGroupView(hwnd, TRUE);
        for (int g = 0; g < table_.groupCount; ++g) {
            const wxString title = wxString(table_.groups[g].title);
            LVGROUP header{};
            header.cbSize = sizeof header;
            header.mask = LVGF_HEADER | LVGF_GROUPID;
            header.pszHeader = const_cast<LPWSTR>(title.wc_str());
            header.iGroupId = g;
            ListView_InsertGroup(hwnd, -1, &header);
        }
    }
#endif
    all->Add(list_, 1, wxEXPAND);

    const int noteH = 3 * lineH;
    note_ = new wxStaticText(panel, wxID_ANY, wxString(table_.note));
    note_->SetMinSize(wxSize(-1, noteH - FromDIP(6)));
    all->Add(note_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    panel->SetSizer(all);

    FillRows(nullptr);
    Refilter(true);

    // Size the window to its columns and rows, capped so it fits beside others.
    const int groupRows = table_.Grouped() ? 2 * table_.groupCount : 0;
    const int listH = std::min(lineH + FromDIP(8) + (table_.RowCount() + groupRows) * (lineH + FromDIP(1)),
                               FromDIP(560));
    SetClientSize(clientW, filterH + listH + noteH);
    SetMinSize(FromDIP(wxSize(260, 180)));

    // Cascade from the assistant's top-left corner, kept on its monitor.
    const int display = wxDisplay::GetFromWindow(owner);
    const wxRect work = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea();
    const wxPoint anchor = owner->GetPosition();
    const wxSize size = GetSize();
    const int step = FromDIP(40) * (kind + 1);
    SetPosition(wxPoint(std::max(work.x, std::min(anchor.x + step, work.GetRight() + 1 - size.x)),
                        std::max(work.y, std::min(anchor.y + step, work.GetBottom() + 1 - size.y))));

    wxToolTip::SetAutoPop(30000);
#ifdef __WXMSW__
    wxToolTip::SetMaxWidth(FromDIP(360));  // also lets the text break at line ends
#endif
    list_->Bind(wxEVT_MOTION, &ReferenceFrame::OnMotion, this);
    list_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        tipRow_ = -2;
        event.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, &ReferenceFrame::OnClose, this);
    Bind(wxEVT_SIZE, &ReferenceFrame::OnSize, this);
#ifdef __WXMSW__
    CreateHeaderTip();
    list_->Bind(wxEVT_LIST_COL_DRAGGING, [this](wxListEvent& event) {
        PlaceHeaderTip();
        event.Skip();
    });
    list_->Bind(wxEVT_LIST_COL_END_DRAG, [this](wxListEvent& event) {
        CallAfter([this] { PlaceHeaderTip(); });
        event.Skip();
    });
#endif
}

// Fills the list with the table's rows, or only those marked in `visible`.
void ReferenceFrame::FillRows(const std::vector<bool>* visible) {
    list_->Freeze();
    list_->DeleteAllItems();
    long item = 0;
    int row = 0;
    for (int g = 0; g < table_.groupCount; ++g) {
        const u3::ref::Group& group = table_.groups[g];
#ifndef __WXMSW__
        // No native groups here: a bold heading row instead.
        if (group.title) {
            list_->InsertItem(item, wxString(group.title));
            list_->SetItemData(item, -1);
            list_->SetItemFont(item, list_->GetFont().Bold());
            ++item;
        }
#endif
        for (int cell = 0; cell < group.cellCount; cell += table_.columnCount, ++row) {
            if (visible && !(*visible)[row]) continue;
            list_->InsertItem(item, wxString(group.cells[cell]));
            for (int c = 1; c < table_.columnCount; ++c)
                list_->SetItem(item, c, wxString(group.cells[cell + c]));
            list_->SetItemData(item, row);
#ifdef __WXMSW__
            if (table_.Grouped()) {
                LVITEMW native{};
                native.mask = LVIF_GROUPID;
                native.iItem = item;
                native.iGroupId = g;
                ListView_SetItem(static_cast<HWND>(list_->GetHWND()), &native);
            }
#endif
            ++item;
        }
    }
    list_->Thaw();
    tipRow_ = -2;
}

void ReferenceFrame::Refilter(bool force) {
    if (!castable_) return;
    std::vector<bool> rows;
    std::wstring whom;
    if (castable_->GetValue() && g_party.live)
        rows = u3::ref::CastableRows(g_party, &whom);
    else if (castable_->GetValue())
        whom = L"No party connected, so every spell is shown";
    const wxString caption(whom);
    if (castableFor_->GetLabelText() != caption) castableFor_->SetLabelText(caption);
    if (force || rows != shownRows_) {
        shownRows_ = rows;
        FillRows(rows.empty() ? nullptr : &rows);
    }
}

void ReferenceFrame::OnMotion(wxMouseEvent& event) {
    event.Skip();
    int flags = 0;
    const long item = list_->HitTest(event.GetPosition(), flags);
    const int row = item == wxNOT_FOUND ? -1 : static_cast<int>(list_->GetItemData(item));
    if (row == tipRow_) return;
    tipRow_ = row;
    // Removing the old tip first restarts the delay for the new row.
    list_->UnsetToolTip();
    const wxString text = u3::ref::UsersText(kind_, row, g_party);
    if (!text.empty()) list_->SetToolTip(text);
}

void ReferenceFrame::OnClose(wxCloseEvent& event) {
    if (!g_appClosing) {
        settings::SaveWindow(PLACEMENT_KEYS[kind_], this);
        settings::SetInt("Reference", OPEN_KEYS[kind_], 0);
        g_closedAt[kind_] = Clock::now();
        g_closedRecently[kind_] = true;
    }
    event.Skip();  // destroys the window
}

void ReferenceFrame::OnSize(wxSizeEvent& event) {
    event.Skip();
#ifndef __WXMSW__
    // Windows wraps static text by itself; GTK needs telling.
    CallAfter([this] {
        note_->SetLabelText(wxString(table_.note));
        note_->Wrap(note_->GetSize().x);
    });
#endif
}

#ifdef __WXMSW__
// A native tooltip over the Classes column header, explaining the letters.
void ReferenceFrame::CreateHeaderTip() {
    const HWND header = ListView_GetHeader(static_cast<HWND>(list_->GetHWND()));
    headerTip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                 0, 0, 0, 0, static_cast<HWND>(GetHWND()), nullptr, nullptr, nullptr);
    if (!headerTip_) return;
    SendMessageW(headerTip_, TTM_SETMAXTIPWIDTH, 0, FromDIP(360));  // also lets the text break at \n
    SendMessageW(headerTip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    static const wxString legend = wxString(u3::ref::CLASS_LEGEND);
    TOOLINFOW tool{};
    tool.cbSize = sizeof tool;
    tool.uFlags = TTF_SUBCLASS;
    tool.hwnd = header;
    tool.uId = 1;
    tool.lpszText = const_cast<LPWSTR>(legend.wc_str());
    Header_GetItemRect(header, table_.classesColumn, &tool.rect);
    SendMessageW(headerTip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

// Keeps the tooltip over the Classes column as columns are resized.
void ReferenceFrame::PlaceHeaderTip() {
    if (!headerTip_) return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof tool;
    tool.hwnd = ListView_GetHeader(static_cast<HWND>(list_->GetHWND()));
    tool.uId = 1;
    Header_GetItemRect(tool.hwnd, table_.classesColumn, &tool.rect);
    SendMessageW(headerTip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
}
#endif

}  // namespace

void Show(Kind kind, wxWindow* owner, bool topmost) {
    if (ReferenceFrame* open = g_frames[kind]) {
        if (open->IsIconized()) open->Iconize(false);
        open->Raise();
        return;
    }
    auto* frame = new ReferenceFrame(kind, owner, topmost);
    g_frames[kind] = frame;
    // Recorded now, so it's reopened next time even if the app never gets to close normally.
    settings::SetInt("Reference", OPEN_KEYS[kind], 1);
    g_closedRecently[kind] = false;
    if (!settings::RestoreWindow(PLACEMENT_KEYS[kind], frame)) frame->Show();
}

void SetTopmost(bool topmost) {
    for (ReferenceFrame* frame : g_frames) {
        if (!frame) continue;
        const long style = frame->GetWindowStyleFlag();
        frame->SetWindowStyleFlag(topmost ? style | wxSTAY_ON_TOP : style & ~wxSTAY_ON_TOP);
    }
}

void UpdateParty(const u3::ref::PartyState& party) {
    g_party = party;
    if (g_frames[SPELLS]) g_frames[SPELLS]->Refilter(false);
}

void RestoreOpenWindows(wxWindow* owner, bool topmost) {
    for (int kind = 0; kind < KIND_COUNT; ++kind)
        if (settings::GetInt("Reference", OPEN_KEYS[kind], 0)) Show(static_cast<Kind>(kind), owner, topmost);
}

void SaveAndCloseAll() {
    g_appClosing = true;
    const auto now = Clock::now();
    for (int kind = 0; kind < KIND_COUNT; ++kind) {
        ReferenceFrame* frame = g_frames[kind];
        if (frame) settings::SaveWindow(PLACEMENT_KEYS[kind], frame);
        const bool justClosed = g_closedRecently[kind] && now - g_closedAt[kind] < CLOSE_ALL_GRACE;
        settings::SetInt("Reference", OPEN_KEYS[kind], frame || justClosed ? 1 : 0);
        if (frame) frame->Destroy();
    }
}

}  // namespace refwin
