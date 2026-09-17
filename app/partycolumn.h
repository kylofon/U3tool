// partycolumn.h -- one party member's box in the main window: name and
// condition, race, hit points with a bar, attributes, experience, food, gold,
// the Carrying list and the gem/key/powder/torch counters.
#pragma once

#include <wx/colour.h>
#include <wx/string.h>

#include <vector>

#include "reader.h"

class CarriedList;
class wxGauge;
class wxSizer;
class wxStaticBox;
class wxStaticText;
class wxWindow;

class PartyColumn {
public:
    // Builds the controls inside a group box on `parent`; returns its sizer.
    wxSizer* Create(wxWindow* parent);

    void Fill(const u3::Character& ch);
    void ShowEmpty();

    // Shows a weapon (or armour) as readied straight away, before the game
    // has been asked; type 0 for nothing readied. The next poll confirms it.
    void ShowEquipped(bool armour, int type);

    wxStaticBox* Box() const { return box_; }
    CarriedList* List() const { return list_; }
    // What each line of the Carrying list holds; shorter than the list for "(nothing)".
    const std::vector<u3::CarriedItem>& Items() const { return items_; }

private:
    enum Row { ROW_HP, ROW_MP, ROW_EXP, ROW_FOOD, ROW_GOLD, ROW_COUNT };

    void SetBar(int max, int pos, int state);
    void ShowCarried();

    wxStaticBox* box_ = nullptr;
    wxStaticText* name_ = nullptr;
    wxStaticText* race_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxGauge* hpBar_ = nullptr;
    wxStaticText* rowValue_[ROW_COUNT] = {};
    wxStaticText* statValue_[4] = {};
    wxStaticText* counterValue_[4] = {};
    CarriedList* list_ = nullptr;

    std::vector<u3::CarriedItem> items_;
    int barMax_ = -1, barPos_ = -1, barState_ = -1;
    bool levelUp_ = false;
    bool empty_ = false;
};
