// partycolumn.cpp -- building and filling a party member's box.
#include "partycolumn.h"

#include <wx/gauge.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#endif

#include <algorithm>
#include <utility>

#include "carriedlist.h"

namespace {

const wxColour COL_GOOD(0, 128, 0);
const wxColour COL_POISONED(170, 110, 0);
const wxColour COL_DEAD(192, 0, 0);
const wxColour COL_ASHES(110, 110, 110);

// Hit point bar colours, as the Windows progress bar states name them.
constexpr int BAR_NORMAL = 1, BAR_RED = 2, BAR_YELLOW = 3;

constexpr int CARRY_LINES = 5;  // the Carrying list scrolls beyond this

const char* const ROW_LABELS[] = {"Hit points", "Magic points", "Experience", "Food", "Gold"};
const char* const STAT_LABELS[] = {"Strength", "Dexterity", "Intelligence", "Wisdom"};
const char* const COUNTER_LABELS[] = {"Gems", "Keys", "Powders", "Torches"};

const wxString LEVEL_UP_TIP =
    wxString::FromUTF8("▲ Lord British will raise this character's maximum hit points by 100 — visit him.");

wxString Num(int v) { return v < 0 ? wxString::FromUTF8("–") : wxString::Format("%d", v); }

void SetText(wxStaticText* text, const wxString& value) {
    if (text->GetLabelText() != value) text->SetLabelText(value);
}

void SetColour(wxWindow* window, const wxColour& colour) {
    if (window->GetForegroundColour() == colour) return;
    window->SetForegroundColour(colour);
    window->Refresh();
}

wxColour StatusColour(char code) {
    switch (code) {
        case 'G': return COL_GOOD;
        case 'D': return COL_DEAD;
        case 'A': return COL_ASHES;
        default: return COL_POISONED;
    }
}

wxStaticText* Label(wxWindow* parent, const wxString& text, long style = 0) {
    return new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, style);
}

}  // namespace

wxSizer* PartyColumn::Create(wxWindow* parent) {
    auto* boxSizer = new wxStaticBoxSizer(wxVERTICAL, parent, " ");
    box_ = boxSizer->GetStaticBox();
    const int lineH = parent->GetCharHeight() + parent->FromDIP(4);
    const int pad = parent->FromDIP(6);
    wxFont bold = parent->GetFont().Bold();

    auto* inner = new wxBoxSizer(wxVERTICAL);
    boxSizer->Add(inner, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

    // A label on the left and a bold value on the right, a line high.
    auto row = [&](const wxString& label, wxStaticText** value) {
        auto* line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(Label(box_, label), 0, wxALIGN_CENTER_VERTICAL);
        line->AddStretchSpacer();
        *value = Label(box_, "", wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
        (*value)->SetFont(bold);
        (*value)->SetMinSize(wxSize(parent->FromDIP(80), -1));
        line->Add(*value, 0, wxALIGN_CENTER_VERTICAL);
        line->SetMinSize(wxSize(-1, lineH));
        inner->Add(line, 0, wxEXPAND);
    };
    // Two columns of label and value, two rows deep.
    auto grid = [&](const char* const* labels, wxStaticText** values) {
        auto* table = new wxGridSizer(2, 2, 0, parent->FromDIP(12));
        for (int k = 0; k < 4; ++k) {
            auto* cell = new wxBoxSizer(wxHORIZONTAL);
            cell->Add(Label(box_, labels[k]), 0, wxALIGN_CENTER_VERTICAL);
            cell->AddStretchSpacer();
            values[k] = Label(box_, "", wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
            values[k]->SetFont(bold);
            values[k]->SetMinSize(wxSize(parent->FromDIP(30), -1));
            cell->Add(values[k], 0, wxALIGN_CENTER_VERTICAL);
            cell->SetMinSize(wxSize(-1, lineH));
            table->Add(cell, 0, wxEXPAND);
        }
        inner->Add(table, 0, wxEXPAND);
    };
    auto rule = [&] {
        inner->Add(new wxStaticLine(box_), 0, wxEXPAND | wxTOP | wxBOTTOM, parent->FromDIP(5));
    };

    // The name, and the condition at its right, level with the name's foot.
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    name_ = Label(box_, "", wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    name_->SetFont(parent->GetFont().Bold().Scaled(1.5f));
    top->Add(name_, 1, wxALIGN_BOTTOM);
    status_ = Label(box_, "", wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
    status_->SetMinSize(wxSize(parent->FromDIP(76), -1));
    top->Add(status_, 0, wxALIGN_BOTTOM | wxBOTTOM, parent->FromDIP(2));
    inner->Add(top, 0, wxEXPAND);

    race_ = Label(box_, "", wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    race_->SetMinSize(wxSize(-1, lineH));
    inner->Add(race_, 0, wxEXPAND | wxBOTTOM, parent->FromDIP(4));

    row(ROW_LABELS[ROW_HP], &rowValue_[ROW_HP]);
    hpBar_ = new wxGauge(box_, wxID_ANY, 1, wxDefaultPosition, wxSize(-1, parent->FromDIP(14)),
                         wxGA_HORIZONTAL | wxGA_SMOOTH);
    inner->Add(hpBar_, 0, wxEXPAND | wxTOP | wxBOTTOM, parent->FromDIP(3));
    row(ROW_LABELS[ROW_MP], &rowValue_[ROW_MP]);
    rule();
    grid(STAT_LABELS, statValue_);
    rule();
    row(ROW_LABELS[ROW_EXP], &rowValue_[ROW_EXP]);
    row(ROW_LABELS[ROW_FOOD], &rowValue_[ROW_FOOD]);
    row(ROW_LABELS[ROW_GOLD], &rowValue_[ROW_GOLD]);
    rule();

    auto* carrying = Label(box_, "Carrying");
    carrying->SetMinSize(wxSize(-1, lineH));
    inner->Add(carrying, 0, wxEXPAND);
    list_ = new CarriedList(box_);
    list_->SetMinSize(wxSize(-1, CARRY_LINES * lineH + parent->FromDIP(4)));
    inner->Add(list_, 0, wxEXPAND | wxBOTTOM, parent->FromDIP(8));
    grid(COUNTER_LABELS, counterValue_);

    ShowEmpty();
    return boxSizer;
}

void PartyColumn::SetBar(int max, int pos, int state) {
    if (max == barMax_ && pos == barPos_ && state == barState_) return;
    barMax_ = max;
    barPos_ = pos;
    barState_ = state;
#ifdef __WXMSW__
    // A themed bar ignores new positions while yellow (paused) or red (error)
    // and animates any growth. So go normal, jump straight to the position by
    // overshooting one and stepping back, then apply the colour.
    const HWND bar = static_cast<HWND>(hpBar_->GetHWND());
    SendMessage(bar, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessage(bar, PBM_SETRANGE32, 0, max + 1);
    SendMessage(bar, PBM_SETPOS, pos + 1, 0);
    SendMessage(bar, PBM_SETPOS, pos, 0);
    SendMessage(bar, PBM_SETRANGE32, 0, max);
    SendMessage(bar, PBM_SETSTATE, state, 0);
#else
    (void)state;  // GTK draws the bar in the theme's colour
    hpBar_->SetRange(max);
    hpBar_->SetValue(pos);
#endif
}

void PartyColumn::ShowEmpty() {
    empty_ = true;
    if (box_->GetLabel() != " ") box_->SetLabel(" ");
    SetText(name_, wxString::FromUTF8("— empty —"));
    SetColour(name_, wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    SetText(race_, "");
    SetText(status_, "");
    SetBar(1, 0, BAR_NORMAL);
    for (wxStaticText* value : rowValue_) SetText(value, "");
    for (int k = 0; k < 4; ++k) {
        SetText(statValue_[k], "");
        SetText(counterValue_[k], "");
    }
    if (levelUp_) {
        levelUp_ = false;
        SetColour(rowValue_[ROW_EXP], wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        rowValue_[ROW_EXP]->UnsetToolTip();
    }
    items_.clear();
    list_->SetLines({});
}

void PartyColumn::Fill(const u3::Character& ch) {
    if (empty_) {
        empty_ = false;
        SetColour(name_, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    }
    // The group box caption carries class, gender and level.
    wxString caption = wxString(ch.klass) + wxString::FromUTF8("  ·  ") + wxString(ch.sex);
    if (ch.level > 0) caption += wxString::Format(wxString::FromUTF8("  ·  Level %d"), ch.level);
    caption.Replace("&", "&&");  // the box treats '&' as a mnemonic
    if (box_->GetLabel() != caption) box_->SetLabel(caption);

    SetText(name_, ch.name.empty() ? wxString("(unnamed)") : wxString(ch.name));
    SetText(race_, ch.race);
    SetText(status_, ch.status);
    SetColour(status_, StatusColour(ch.statusCode));

    if (ch.hp >= 0 && ch.maxHp > 0) {
        const int pos = std::min(ch.hp, ch.maxHp);
        const int state = pos * 2 > ch.maxHp ? BAR_NORMAL : (pos * 4 > ch.maxHp ? BAR_YELLOW : BAR_RED);
        SetText(rowValue_[ROW_HP], wxString::Format("%d / %d", ch.hp, ch.maxHp));
        SetBar(ch.maxHp, pos, state);
    } else {
        SetText(rowValue_[ROW_HP], "?");
        SetBar(1, 0, BAR_RED);
    }

    SetText(rowValue_[ROW_MP], Num(ch.mp));
    SetText(rowValue_[ROW_EXP], ch.canLevelUp ? wxString::FromUTF8("▲ ") + Num(ch.exp) : Num(ch.exp));
    if (ch.canLevelUp != levelUp_) {
        levelUp_ = ch.canLevelUp;
        SetColour(rowValue_[ROW_EXP], levelUp_ ? COL_GOOD : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        if (levelUp_)
            rowValue_[ROW_EXP]->SetToolTip(LEVEL_UP_TIP);
        else
            rowValue_[ROW_EXP]->UnsetToolTip();
    }
    SetText(rowValue_[ROW_FOOD], Num(ch.food));
    SetText(rowValue_[ROW_GOLD], Num(ch.gold));

    const int stats[4] = {ch.strength, ch.dexterity, ch.intelligence, ch.wisdom};
    const int counters[4] = {ch.gems, ch.keys, ch.powders, ch.torches};
    for (int k = 0; k < 4; ++k) {
        SetText(statValue_[k], Num(stats[k]));
        SetText(counterValue_[k], Num(counters[k]));
    }

    std::vector<CarriedList::Line> lines;
    for (const u3::CarriedItem& item : ch.carried)
        lines.push_back({wxString(item.name) + wxString::FromUTF8("  × ") + wxString::Format("%d", item.count), true,
                         item.equipped, item.armour});
    if (lines.empty()) lines.push_back({"(nothing)"});
    items_ = ch.carried;
    list_->SetLines(std::move(lines));
}
