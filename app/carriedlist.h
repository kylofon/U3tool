// carriedlist.h -- a party member's Carrying list: equipped items in bold
// with a grey "readied" / "worn" tag, and items that can be dragged onto
// another party member.
#pragma once

#include <wx/vlbox.h>

#include <functional>
#include <vector>

class CarriedList : public wxVListBox {
public:
    struct Line {
        wxString text;
        bool item = false;  // false for "(nothing)"
        bool equipped = false;
        bool armour = false;

        bool operator==(const Line& o) const {
            return text == o.text && item == o.item && equipped == o.equipped && armour == o.armour;
        }
    };

    explicit CarriedList(wxWindow* parent);

    void SetLines(std::vector<Line> lines);  // repaints only if something changed
    int LineAt(const wxPoint& clientPoint) const;  // wxNOT_FOUND off the lines

    // Dragging a line: onDragStart says whether it may be dragged, onDragOver
    // whether it may be dropped at a screen point, onDrop takes it there.
    std::function<bool(int line)> onDragStart;
    std::function<bool(const wxPoint& screen)> onDragOver;
    std::function<void(const wxPoint& screen)> onDrop;

protected:
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override;
    wxCoord OnMeasureItem(size_t n) const override;

private:
    void OnLeftDown(wxMouseEvent& event);
    void OnMotion(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnCaptureLost(wxMouseCaptureLostEvent& event);
    void EndDrag();

    std::vector<Line> lines_;
    int pressedLine_ = wxNOT_FOUND;
    wxPoint pressedAt_;
    bool dragging_ = false;
};
