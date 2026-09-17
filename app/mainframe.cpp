// mainframe.cpp -- the main window and its polling thread.
#include "mainframe.h"

#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>
#include <wx/textctrl.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

#include "carriedlist.h"
#include "dialogs.h"
#include "icon.h"
#include "mapframe.h"
#include "referenceframe.h"
#include "refdata.h"
#include "settings.h"

namespace {

constexpr int ID_FOOD = wxID_HIGHEST + 1, ID_FASTER = ID_FOOD + 1, ID_SLOWER = ID_FOOD + 2,
              ID_PAUSE = ID_FOOD + 3, ID_NORMAL = ID_FOOD + 4, ID_RESCAN = ID_FOOD + 5, ID_NEXT = ID_FOOD + 6,
              ID_RAW = ID_FOOD + 7, ID_DEBUG_INFO = ID_FOOD + 8, ID_EQUIP = ID_FOOD + 9, ID_ABOUT = ID_FOOD + 10;
constexpr int ID_POOL_BASE = wxID_HIGHEST + 100;       // + party member
constexpr int ID_REVIVE_BASE = wxID_HIGHEST + 110;     // + party member
constexpr int ID_HEAL_BASE = wxID_HIGHEST + 120;       // + party member
constexpr int ID_CURE_BASE = wxID_HIGHEST + 130;       // + party member
constexpr int ID_REFERENCE_BASE = wxID_HIGHEST + 140;  // + u3::ref::Kind
constexpr int ID_MAPS_BASE = wxID_HIGHEST + 150;       // + mapwin::Kind

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

// Idle waits offered by the Game speed menu, fastest first.
const int PASS_STEPS[] = {1, 2, 3, u3::NORMAL_PASS_SECONDS, 10, 15, 30, u3::MAX_PASS_SECONDS};
constexpr int STEP_COUNT = sizeof PASS_STEPS / sizeof PASS_STEPS[0];
constexpr int NORMAL_STEP = 3;

constexpr int RAW_HEIGHT = 190;
constexpr int SPEED_FIELD_WIDTH = 400;

wxString Num(int v) { return v < 0 ? wxString::FromUTF8("–") : wxString::Format("%d", v); }

// Menus treat '&' as a mnemonic marker, so double any in character names.
wxString MenuEscape(wxString s) {
    s.Replace("&", "&&");
    return s;
}

// A member menu line: the name, and details at the right edge where the
// platform supports it.
wxString MemberLine(const std::wstring& name, const wxString& info) {
#ifdef __WXMSW__
    return MenuEscape(name) + "\t" + info;
#else
    return MenuEscape(name) + wxString::FromUTF8(" — ") + info;
#endif
}

bool IsDead(const u3::Character& ch) { return ch.statusCode == 'D' || ch.statusCode == 'A'; }

void SetGood(u3::Character& ch) {
    ch.statusCode = 'G';
    ch.status = L"Good";
}

// Hands `count` of an item over, keeping the game's order: weapons by type,
// then armour, with anything in use on its own line.
void HandOver(u3::Character& giver, u3::Character& taker, const u3::CarriedItem& item, int count) {
    for (auto line = giver.carried.begin(); line != giver.carried.end(); ++line) {
        if (line->armour != item.armour || line->type != item.type || line->equipped) continue;
        line->count -= std::min(count, line->count);
        if (line->count == 0) giver.carried.erase(line);
        break;
    }
    for (u3::CarriedItem& line : taker.carried) {
        if (line.armour != item.armour || line.type != item.type || line.equipped) continue;
        line.count = std::min(line.count + count, u3::MAX_BCD1);
        return;
    }
    auto at = taker.carried.begin();
    while (at != taker.carried.end() &&
           (at->armour < item.armour || (at->armour == item.armour && at->type < item.type)))
        ++at;
    u3::CarriedItem fresh = item;
    fresh.count = count;
    fresh.equipped = false;
    taker.carried.insert(at, fresh);
}

wxString DescribeSpeed(const u3::GameSpeed& speed) {
    if (speed.paused) return wxString::FromUTF8("paused — turns pass only when you act");
    const char* pace = speed.passSeconds == u3::NORMAL_PASS_SECONDS  ? "normal"
                       : speed.passSeconds < u3::NORMAL_PASS_SECONDS ? "accelerated"
                                                                      : "slowed down";
    return wxString::Format(wxString::FromUTF8("%s — an idle turn passes after %d s"), pace, speed.passSeconds);
}

wxString HexDump(const uint8_t* d) {
    wxString out = "Party header (18 bytes), then 4 x 64-byte character records\n\n";
    for (size_t off = 0; off < u3::PARTY_SIZE; off += 16) {
        const size_t n = std::min<size_t>(16, u3::PARTY_SIZE - off);
        const wxString tag = off < u3::HEADER_SIZE
                                 ? wxString("hdr")
                                 : wxString::Format("c%u+%02X", static_cast<unsigned>((off - u3::HEADER_SIZE) / u3::RECORD_SIZE),
                                                    static_cast<unsigned>((off - u3::HEADER_SIZE) % u3::RECORD_SIZE));
        wxString hex, text;
        for (size_t i = 0; i < 16; ++i) {
            if (i < n) {
                const uint8_t b = d[off + i];
                hex += wxString::Format("%02x ", b);
                text += (b >= 32 && b < 127) ? wxUniChar(b) : wxUniChar('.');
            } else {
                hex += "   ";
            }
        }
        out += wxString::Format("%04X  %-7s  ", static_cast<unsigned>(off), tag) + hex + " " + text + "\n";
    }
    return out;
}

}  // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, wxString(APP_TITLE) + " (Not connected)"),
      step_(NORMAL_STEP),
      wantSeconds_(u3::NORMAL_PASS_SECONDS) {
    topmost_ = settings::GetInt("Preferences", "AlwaysOnTop", 1) != 0;
    if (topmost_) SetWindowStyleFlag(GetWindowStyleFlag() | wxSTAY_ON_TOP);
    SetIcons(AppIcons());

    panel_ = new wxPanel(this);
    auto* all = new wxBoxSizer(wxVERTICAL);
    const int margin = FromDIP(10), gap = FromDIP(8);

    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < 4; ++i) {
        wxSizer* column = columns_[i].Create(panel_);
        columns->Add(column, 1, wxEXPAND | (i ? wxLEFT : 0), gap);

        CarriedList* list = columns_[i].List();
        list->onDragStart = [this, i](int line) {
            if (!live_ || party_.count < 2 || i >= party_.count ||
                line >= static_cast<int>(columns_[i].Items().size()))
                return false;
            dragFrom_ = i;
            dragItem_ = columns_[i].Items()[line];
            SetStatus(0, "Drop on another party member to hand the item over.");
            return true;
        };
        list->onDragOver = [this](const wxPoint& screen) { return DropTarget(screen) >= 0; };
        list->onDrop = [this](const wxPoint& screen) {
            const int from = dragFrom_, to = DropTarget(screen);
            const u3::CarriedItem item = dragItem_;
            dragFrom_ = -1;
            // Let the list finish the drag before a pop-up takes over.
            if (to >= 0) CallAfter([this, from, to, item] { GiveItems(from, to, item); });
        };
        list->Bind(wxEVT_CONTEXT_MENU, [this, i](wxContextMenuEvent& event) { ShowItemMenu(i, event.GetPosition()); });
    }
    columnsSizer_ = columns;
    all->Add(columns, 0, wxEXPAND | wxALL, margin);

    wait_ = new wxStaticText(panel_, wxID_ANY, "Waiting for connection, is the game running?", wxDefaultPosition,
                             wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    wait_->SetFont(GetFont().Bold().Scaled(1.5f));
    wait_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    waitSizer_ = new wxBoxSizer(wxVERTICAL);
    waitSizer_->AddStretchSpacer();
    waitSizer_->Add(wait_, 0, wxALIGN_CENTER_HORIZONTAL);
    waitSizer_->AddStretchSpacer();
    all->Add(waitSizer_, 1, wxEXPAND);

    raw_ = new wxTextCtrl(panel_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                          wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxHSCROLL);
    raw_->SetFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE)
#ifdef __WXMSW__
                      .FaceName("Consolas")
#endif
    );
    all->Add(raw_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margin);
    raw_->Hide();
    panel_->SetSizer(all);

    CreateStatusBar(2);
    const int widths[2] = {-1, FromDIP(SPEED_FIELD_WIDTH)};
    SetStatusWidths(2, widths);
    SetStatus(0, "Ready.");
    CreateMenus();

    // Fit the height to the party columns; never narrower than they read well.
    const int columnsH = columns->GetMinSize().y;
    SetClientSize(FromDIP(1080), columnsH + 2 * margin + GetStatusBar()->GetSize().y);
    SetMinClientSize(wxSize(FromDIP(900), columnsH + 2 * margin + GetStatusBar()->GetSize().y));
    ShowParty(false);  // until the first snapshot finds a party

    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    Bind(wxEVT_MENU_OPEN, &MainFrame::OnMenuOpen, this);

    const wxString host = settings::GetString("Staging", "Host", "127.0.0.1");
    const int port = settings::GetInt("Staging", "Port", 8086);
    worker_ = std::thread([this, host = host.ToStdWstring(), port] { Work(host, port); });
}

MainFrame::~MainFrame() { StopWorker(); }

void MainFrame::CreateMenus() {
    auto* file = new wxMenu;
    file->Append(wxID_PREFERENCES, wxString::FromUTF8("&Preferences…"));
    file->AppendSeparator();
    file->Append(wxID_EXIT, "&Quit\tAlt+F4");

    poolMenu_ = new wxMenu;
    actionsMenu_ = new wxMenu;
    actionsMenu_->Append(ID_FOOD, "&Distribute food");
    poolItem_ = actionsMenu_->AppendSubMenu(poolMenu_, "&Pool gold");

    speedMenu_ = new wxMenu;
    speedMenu_->Append(ID_FASTER, "&Accelerate");
    speedMenu_->Append(ID_SLOWER, "&Slow down");
    speedMenu_->AppendCheckItem(ID_PAUSE, "&Pause");
    speedMenu_->AppendSeparator();
    speedMenu_->Append(ID_NORMAL, "&Normal speed");

    auto* reference = new wxMenu;
    reference->Append(ID_REFERENCE_BASE + u3::ref::WEAPONS, "&Weapons");
    reference->Append(ID_REFERENCE_BASE + u3::ref::ARMOUR, "&Armour");
    reference->Append(ID_REFERENCE_BASE + u3::ref::SPELLS, "&Spells");
    auto* maps = new wxMenu;
    maps->Append(ID_MAPS_BASE + mapwin::WORLD, "&World");
    maps->Append(ID_MAPS_BASE + mapwin::DUNGEONS, "&Dungeons");
    reference->AppendSeparator();
    reference->AppendSubMenu(maps, "&Maps");

    reviveMenu_ = new wxMenu;
    healMenu_ = new wxMenu;
    cureMenu_ = new wxMenu;
    cheatMenu_ = new wxMenu;
    cheatMenu_->AppendSubMenu(reviveMenu_, "&Revive");
    cheatMenu_->AppendSubMenu(healMenu_, "&Full health");
    cheatMenu_->AppendSubMenu(cureMenu_, "C&ure");

    debugMenu_ = new wxMenu;
    debugMenu_->Append(ID_DEBUG_INFO, "Not connected")->Enable(false);
    debugMenu_->AppendSeparator();
    debugMenu_->Append(ID_RESCAN, "&Rescan memory");
    debugMenu_->Append(ID_NEXT, "&Next source");
    debugMenu_->AppendSeparator();
    debugMenu_->AppendCheckItem(ID_RAW, "Raw &bytes");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(actionsMenu_, "&Actions");
    bar->Append(speedMenu_, "Game &speed");
    bar->Append(reference, "&Reference");
    bar->Append(cheatMenu_, "&Cheat");
    bar->Append(debugMenu_, "&Debug");
#ifndef __WXMSW__
    auto* about = new wxMenu;
    about->Append(ID_ABOUT, wxString::FromUTF8("&About Ultima III Assistant…"));
    bar->Append(about, "A&bout");
#endif
    SetMenuBar(bar);
#ifdef __WXMSW__
    // A top-level About item that acts straight away, which wxWidgets menus
    // can't express; MSWWindowProc handles its command.
    const HMENU native = static_cast<HMENU>(bar->GetHMenu());
    AppendMenuW(native, MF_STRING, ID_ABOUT, L"A&bout");
    DrawMenuBar(static_cast<HWND>(GetHWND()));
#endif
    RebuildMemberMenus();

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ShowAbout(this); }, ID_ABOUT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        bool on = topmost_;
        if (ShowPreferences(this, on)) SetTopmost(on);
    }, wxID_PREFERENCES);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        RequestAction(ACTION_FOOD, wxString::FromUTF8("Distributing food…"));
        Predict([](u3::Party& party) {
            int total = 0;
            for (int i = 0; i < party.count; ++i) total += std::max(party.chars[i].food, 0);
            // An even split, with any remainder going one apiece to the first members.
            const int share = total / party.count, extra = total % party.count;
            for (int i = 0; i < party.count; ++i) party.chars[i].food = share + (i < extra ? 1 : 0);
        });
    }, ID_FOOD);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetSpeed(step_ - 1, false); }, ID_FASTER);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetSpeed(step_ + 1, false); }, ID_SLOWER);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetSpeed(step_, !paused_); }, ID_PAUSE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SetSpeed(NORMAL_STEP, false); }, ID_NORMAL);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        SetStatus(0, wxString::FromUTF8("Scanning DOSBox memory…"));
        wantRescan_ = true;
        Wake();
    }, ID_RESCAN);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wantNext_ = true;
        Wake();
    }, ID_NEXT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleRaw(); }, ID_RAW);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        refwin::Show(static_cast<refwin::Kind>(e.GetId() - ID_REFERENCE_BASE), this, topmost_);
    }, ID_REFERENCE_BASE, ID_REFERENCE_BASE + u3::ref::KIND_COUNT - 1);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        mapwin::Show(static_cast<mapwin::Kind>(e.GetId() - ID_MAPS_BASE), this, topmost_);
    }, ID_MAPS_BASE, ID_MAPS_BASE + mapwin::KIND_COUNT - 1);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        const int target = e.GetId() - ID_POOL_BASE;
        RequestAction(ACTION_POOL + target, wxString::FromUTF8("Pooling gold…"));
        Predict([target](u3::Party& party) {
            // Nobody can hold more than 9999, so the rest stays with the others.
            int room = u3::MAX_BCD2 - std::max(party.chars[target].gold, 0), moved = 0;
            for (int i = 0; i < party.count && moved < room; ++i) {
                if (i == target) continue;
                const int take = std::min(std::max(party.chars[i].gold, 0), room - moved);
                party.chars[i].gold -= take;
                moved += take;
            }
            party.chars[target].gold += moved;
        });
    }, ID_POOL_BASE, ID_POOL_BASE + 3);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        const int member = e.GetId() - ID_REVIVE_BASE;
        RequestAction(ACTION_REVIVE + member, wxString::FromUTF8("Reviving…"));
        Predict([member](u3::Party& party) {
            u3::Character& ch = party.chars[member];
            SetGood(ch);
            ch.hp = ch.maxHp;
        });
    }, ID_REVIVE_BASE, ID_REVIVE_BASE + 3);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        const int member = e.GetId() - ID_HEAL_BASE;
        RequestAction(ACTION_HEAL + member, wxString::FromUTF8("Healing…"));
        Predict([member](u3::Party& party) { party.chars[member].hp = party.chars[member].maxHp; });
    }, ID_HEAL_BASE, ID_HEAL_BASE + 3);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        const int member = e.GetId() - ID_CURE_BASE;
        RequestAction(ACTION_CURE + member, wxString::FromUTF8("Curing…"));
        Predict([member](u3::Party& party) { SetGood(party.chars[member]); });
    }, ID_CURE_BASE, ID_CURE_BASE + 3);
}

#ifdef __WXMSW__
WXLRESULT MainFrame::MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) {
    if (msg == WM_COMMAND && LOWORD(wParam) == ID_ABOUT && lParam == 0) {
        CallAfter([this] { ShowAbout(this); });
        return 0;
    }
    return wxFrame::MSWWindowProc(msg, wParam, lParam);
}
#endif

void MainFrame::RestoreLayout() {
    if (!settings::RestoreWindow("Main", this)) Show();
    refwin::RestoreOpenWindows(this, topmost_);
    mapwin::RestoreOpenWindows(this, topmost_);
    Raise();
}

// Fills the member sub-menus with one item per party member, greyed out
// where the action doesn't apply.
void MainFrame::RebuildMemberMenus() {
    auto rebuild = [this](wxMenu* menu, int baseId, auto describe) {
        while (menu->GetMenuItemCount() > 0) menu->Destroy(menu->FindItemByPosition(0));
        if (live_) {
            for (int i = 0; i < party_.count && i < 4; ++i) {
                const u3::Character& ch = party_.chars[i];
                wxString info;
                const bool applies = describe(ch, info);
                menu->Append(baseId + i, MemberLine(ch.name, info))->Enable(applies);
            }
        }
        if (menu->GetMenuItemCount() == 0) menu->Append(wxID_ANY, "(no party)")->Enable(false);
    };
    rebuild(poolMenu_, ID_POOL_BASE, [](const u3::Character& ch, wxString& info) {
        info = Num(ch.gold) + " gold";
        return true;
    });
    rebuild(reviveMenu_, ID_REVIVE_BASE, [](const u3::Character& ch, wxString& info) {
        info = ch.status;
        return IsDead(ch);
    });
    rebuild(healMenu_, ID_HEAL_BASE, [](const u3::Character& ch, wxString& info) {
        if (IsDead(ch)) {
            info = ch.status;
            return false;
        }
        info = Num(ch.hp) + " / " + Num(ch.maxHp) + " HP";
        return ch.hp >= 0 && ch.maxHp > 0 && ch.hp < ch.maxHp;
    });
    rebuild(cureMenu_, ID_CURE_BASE, [](const u3::Character& ch, wxString& info) {
        info = ch.status;
        return ch.statusCode == 'P';
    });
}

void MainFrame::OnMenuOpen(wxMenuEvent& event) {
    const wxMenu* menu = event.GetMenu();
    if (menu == actionsMenu_ || menu == poolMenu_ || menu == cheatMenu_ || menu == reviveMenu_ ||
        menu == healMenu_ || menu == cureMenu_) {
        // Both actions need a live party of at least two.
        const bool party = live_ && party_.count >= 2;
        actionsMenu_->Enable(ID_FOOD, party);
        poolItem_->Enable(party);
        RebuildMemberMenus();
    } else if (menu == speedMenu_) {
        speedMenu_->Enable(ID_FASTER, paused_ || step_ > 0);
        speedMenu_->Enable(ID_SLOWER, paused_ || step_ < STEP_COUNT - 1);
        speedMenu_->Enable(ID_NORMAL, paused_ || step_ != NORMAL_STEP);
        speedMenu_->Check(ID_PAUSE, paused_);
    } else if (menu == debugMenu_) {
        debugMenu_->SetLabel(ID_DEBUG_INFO, MenuEscape(debugInfo_));
        // Only worth cycling when the scan found more than one copy.
        const bool several = sourceCount_ > 1;
        debugMenu_->SetLabel(ID_NEXT, several ? wxString::Format("&Next source (%u of %u)",
                                                                 static_cast<unsigned>(sourceIndex_ + 1),
                                                                 static_cast<unsigned>(sourceCount_))
                                              : wxString("&Next source"));
        debugMenu_->Enable(ID_NEXT, several);
        debugMenu_->Check(ID_RAW, showRaw_);
    }
    event.Skip();
}

void MainFrame::SetStatus(int field, const wxString& text) {
    if (statusText_[field] == text) return;
    statusText_[field] = text;
#ifdef __WXMSW__
    // Two tabs right-align the text in a Windows status bar.
    SetStatusText(field == 1 ? "\t\t" + text : text, field);
#else
    SetStatusText(text, field);
#endif
}

void MainFrame::ShowParty(bool show) {
    if (show == partyShown_) return;
    partyShown_ = show;
    panel_->GetSizer()->Show(columnsSizer_, show, true);
    panel_->GetSizer()->Show(waitSizer_, !show, true);
    raw_->Show(show && showRaw_);
    panel_->Layout();
}

void MainFrame::RenderSpeed(const Snapshot& s) {
    wxString text = "Game speed: ";
    if (s.speed.sites == 0)
        text += DescribeSpeed(u3::GameSpeed{PASS_STEPS[step_], paused_}) + " (waiting for the game)";
    else {
        text += DescribeSpeed(s.speed.actual);
        if (!s.speed.applied) text += wxString::FromUTF8(" — ") + wxString(s.speed.error);
    }
    SetStatus(1, text);
}

void MainFrame::Render(const Snapshot& s) {
    live_ = s.ok;
    sourceCount_ = s.ok ? s.candidates : 0;
    sourceIndex_ = s.index;
    const wxString title = wxString(APP_TITLE) + (s.ok ? " (Connected)" : " (Not connected)");
    if (GetTitle() != title) SetTitle(title);
    ShowParty(s.ok);
    RenderSpeed(s);
    mapwin::UpdateLocation(s.ok && s.hasLocation ? s.location : u3::Location{}, s.gameFolder);

    const wxString source = s.pid ? wxString::Format("%s (pid %lu)", wxString(s.exe), static_cast<unsigned long>(s.pid))
                                  : wxString(s.exe);
    if (!s.ok) {
        debugInfo_ = s.exe.empty() ? wxString("Not connected") : source + ", no party found";
        // Say why once, rather than overwriting later messages on every poll.
        const wxString problem = s.error.empty() ? wxString::FromUTF8("Waiting for a party in memory…") : wxString(s.error);
        if (problem != problem_) SetStatus(0, problem_ = problem);
        for (PartyColumn& column : columns_) column.ShowEmpty();
        refwin::UpdateParty(u3::ref::PartyStateOf(nullptr, s.combatTurn));
        return;
    }
    if (!problem_.empty()) {
        problem_.clear();
        SetStatus(0, "Connected to " + wxString(s.exe) + ".");
    }

    party_ = u3::DecodeParty(s.raw.data());
    refwin::UpdateParty(u3::ref::PartyStateOf(&party_, s.combatTurn));
    debugInfo_ = source + " @ " + wxString::Format("0x%llX", static_cast<unsigned long long>(s.address));

    for (int i = 0; i < 4; ++i) {
        const u3::Character& ch = party_.chars[i];
        if (i < party_.count && ch.present)
            columns_[i].Fill(ch);
        else
            columns_[i].ShowEmpty();
    }

    lastRaw_ = s.raw;
    haveRaw_ = true;
    UpdateRaw();
}

void MainFrame::UpdateRaw() {
    if (!showRaw_ || !haveRaw_) return;
    const wxString hex = HexDump(lastRaw_.data());
    if (hex == lastHex_) return;
    lastHex_ = hex;
#ifdef __WXMSW__
    const HWND edit = static_cast<HWND>(raw_->GetHWND());
    const LRESULT first = SendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    raw_->ChangeValue(hex);
    SendMessage(edit, EM_LINESCROLL, 0, first);
#else
    const long at = raw_->GetInsertionPoint();
    raw_->ChangeValue(hex);
    raw_->SetInsertionPoint(std::min(at, raw_->GetLastPosition()));
#endif
}

void MainFrame::ToggleRaw() {
    showRaw_ = !showRaw_;
    raw_->Show(showRaw_ && partyShown_);
    lastHex_.clear();
    UpdateRaw();
    // Grow or shrink the window by the pane's height so the party columns keep their size.
    const int delta = FromDIP(RAW_HEIGHT) + FromDIP(8);
    const wxSize min = GetMinClientSize();
    SetMinClientSize(wxSize(min.x, min.y + (showRaw_ ? delta : -delta)));
    if (!IsMaximized()) {
        const wxSize size = GetSize();
        SetSize(wxSize(size.x, size.y + (showRaw_ ? delta : -delta)));
    }
    panel_->Layout();
}

void MainFrame::RequestAction(int action, const wxString& pending) {
    SetStatus(0, pending);
    action_ = action;
    Wake();
}

// Accelerate and Slow down also resume from a pause.
void MainFrame::SetSpeed(int step, bool paused) {
    step_ = std::min(std::max(step, 0), STEP_COUNT - 1);
    paused_ = paused;
    wantSeconds_ = PASS_STEPS[step_];
    wantPaused_ = paused;
    Wake();
}

void MainFrame::SetTopmost(bool on) {
    topmost_ = on;
    const long style = GetWindowStyleFlag();
    SetWindowStyleFlag(on ? style | wxSTAY_ON_TOP : style & ~wxSTAY_ON_TOP);
    refwin::SetTopmost(on);
    mapwin::SetTopmost(on);
    settings::SetInt("Preferences", "AlwaysOnTop", on ? 1 : 0);
}

// The other live party member whose box is under a screen point, or -1.
int MainFrame::DropTarget(const wxPoint& screen) const {
    if (!live_ || dragFrom_ < 0) return -1;
    for (int i = 0; i < party_.count && i < 4; ++i)
        if (i != dragFrom_ && columns_[i].Box()->GetScreenRect().Contains(screen)) return i;
    return -1;
}

void MainFrame::GiveItems(int from, int to, const u3::CarriedItem& item) {
    if (!live_ || !haveRaw_) return;
    u3::ItemMove move;
    move.from = from;
    move.to = to;
    move.armour = item.armour;
    move.type = item.type;

    std::wstring why;
    const int movable = u3::MovableItems(lastRaw_.data(), move, &why);
    if (movable == 0) {
        wxMessageBox(why, APP_TITLE, wxOK | wxICON_WARNING, this);
        return;
    }
    // Offer no more than the dragged line shows: the equipped item and its
    // spares are separate lines.
    move.count = std::min(movable, item.count);
    if (item.count > 1) {
        move.count = AskCount(this,
                              wxString::Format("Move how many %s from %s to %s?", wxString(item.name),
                                               wxString(party_.chars[from].name), wxString(party_.chars[to].name)),
                              move.count);
        if (move.count == 0) return;
    }
    RequestAction(PackMove(move), wxString::FromUTF8("Moving items…"));
    Predict([&move, &item](u3::Party& party) {
        HandOver(party.chars[move.from], party.chars[move.to], item, move.count);
    });
}

// Right-click on a carried item: Ready / Wear it, or put it away.
void MainFrame::ShowItemMenu(int member, const wxPoint& screen) {
    CarriedList* list = columns_[member].List();
    int line;
    wxPoint at;
    if (screen == wxDefaultPosition) {  // from the keyboard: use the selected line
        line = list->GetSelection();
        at = wxPoint(FromDIP(16), line >= 0 ? list->GetItemRect(line).GetBottom() : 0);
    } else {
        at = list->ScreenToClient(screen);
        line = list->LineAt(at);
    }
    const std::vector<u3::CarriedItem>& items = columns_[member].Items();
    if (!live_ || !haveRaw_ || member >= party_.count || line < 0 || line >= static_cast<int>(items.size())) return;

    const u3::CarriedItem item = items[line];
    u3::Equip equip;
    equip.member = member;
    equip.armour = item.armour;
    equip.type = item.equipped ? 0 : item.type;
    const wxString label = (item.equipped ? (item.armour ? "&Take off " : "&Put away ")
                                          : (item.armour ? "&Wear " : "&Ready ")) +
                           wxString(item.name);
    wxString problem = u3::EquipProblem(lastRaw_.data(), equip);
    // A spare of the type already in use has nothing left to ready or wear.
    if (!item.equipped)
        for (const u3::CarriedItem& other : items)
            if (other.equipped && other.armour == item.armour && other.type == item.type)
                problem = item.armour ? "One is already being worn." : "One is already readied.";

    wxMenu menu;
    menu.Append(ID_EQUIP, label)->Enable(problem.empty());
    if (!problem.empty()) menu.Append(wxID_ANY, MenuEscape(problem))->Enable(false);
    list->SetSelection(line);
    const int chosen = list->GetPopupMenuSelectionFromUser(menu, at);
    list->SetSelection(wxNOT_FOUND);
    if (chosen != ID_EQUIP) return;
    RequestAction(PackEquip(equip),
                  item.equipped ? wxString::FromUTF8("Unequipping…") : wxString::FromUTF8("Equipping…"));
    // Show it at once: waiting for the next poll looks as if nothing happened.
    columns_[member].ShowEquipped(equip.armour, equip.type);
}

void MainFrame::Predict(const std::function<void(u3::Party&)>& change) {
    if (!live_) return;
    change(party_);
    for (int i = 0; i < party_.count && i < 4; ++i) columns_[i].Fill(party_.chars[i]);
}

// Remembers where every window is and which are open, for the next run.
void MainFrame::SaveLayout() {
    settings::SaveWindow("Main", this);
    refwin::SaveAndCloseAll();
    mapwin::SaveAndCloseAll();
}

void MainFrame::OnClose(wxCloseEvent& event) {
    SaveLayout();
    StopWorker();  // also puts the game's normal speed back
    event.Skip();  // destroys the window
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void MainFrame::Wake() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
    }
    wakeUp_.notify_one();
}

void MainFrame::StopWorker() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wakeUp_.notify_one();
    worker_.join();
}

void MainFrame::Work(const std::wstring& host, int port) {
    u3::DosBoxReader reader(host, port);
    u3::PartyBytes previous{};
    uint64_t previousAt = 0;  // where `previous` was read from; 0 when there's nothing to compare
    std::wstring gameFolder;  // looked up once per emulator
    uint64_t gameFolderSession = 0;

    auto report = [this](u3::ActionResult result) {
        CallAfter([this, result] {
            SetStatus(0, result.message);
            if (!result.ok) wxMessageBox(result.message, APP_TITLE, wxOK | wxICON_WARNING, this);
        });
    };

    for (;;) {
        if (wantRescan_.exchange(false) && reader.Attach()) reader.Scan();
        if (wantNext_.exchange(false)) reader.NextCandidate();

        if (const int action = action_.exchange(0)) {
            u3::ActionResult result;
            u3::PartyBytes current;
            if (!reader.Poll(current))
                result.message = reader.lastError.empty() ? L"No party in memory." : reader.lastError;
            else if (action & ACTION_EQUIP)
                result = reader.SetEquipped(UnpackEquip(action));
            else if (action & ACTION_MOVE)
                result = reader.MoveItems(UnpackMove(action));
            else if (action >= ACTION_CURE && action < ACTION_CURE + 4)
                result = reader.Cure(action - ACTION_CURE);
            else if (action >= ACTION_HEAL && action < ACTION_HEAL + 4)
                result = reader.FullHealth(action - ACTION_HEAL);
            else if (action >= ACTION_REVIVE && action < ACTION_REVIVE + 4)
                result = reader.Revive(action - ACTION_REVIVE);
            else if (action == ACTION_FOOD)
                result = reader.DistributeFood();
            else
                result = reader.PoolGold(action - ACTION_POOL);
            report(result);
        }

        auto snap = std::make_shared<Snapshot>();
        snap->ok = reader.Poll(snap->raw);
        if (snap->ok) {
            // The game's shops unequip a character's weapon or armour on any
            // sale. Put it back when they still own it, comparing with the last poll.
            const uint64_t at = reader.Address();
            if (previousAt == at && reader.canWrite) {
                const std::vector<u3::Equip> lost = u3::EquipmentLostToSale(previous.data(), snap->raw.data());
                for (const u3::Equip& equip : lost) {
                    u3::ActionResult result = reader.SetEquipped(equip);
                    if (result.ok) result.message = L"Re-equipped after the sale: " + result.message;
                    report(result);
                }
                if (!lost.empty()) reader.Poll(snap->raw);  // show the restored equipment straight away
            }
            previous = snap->raw;
            previousAt = at;

            snap->combatTurn = reader.CombatTurn();
            snap->hasLocation = reader.ReadLocation(snap->raw, snap->location);
            if (reader.Session() != gameFolderSession) {
                gameFolderSession = reader.Session();
                gameFolder = reader.GameFolder();
            }
            snap->gameFolder = gameFolder;
            snap->speed = reader.SyncSpeed(u3::GameSpeed{wantSeconds_, wantPaused_});
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
        CallAfter([this, snap] { Render(*snap); });

        // Poll briskly while live; back off while there's nothing to find. A
        // menu request wakes the loop straight away.
        std::unique_lock<std::mutex> lock(mutex_);
        wakeUp_.wait_for(lock, std::chrono::milliseconds(ok ? 250 : 1000), [this] { return stop_ || woken_; });
        if (stop_) break;
        woken_ = false;
    }

    // Don't leave the game paused or slowed once nothing is showing it.
    reader.SyncSpeed(u3::GameSpeed{}, false);
}
