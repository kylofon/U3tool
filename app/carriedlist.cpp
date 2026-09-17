// carriedlist.cpp -- drawing and dragging the Carrying list.
#include "carriedlist.h"

#include <wx/control.h>
#include <wx/dc.h>
#include <wx/settings.h>
#include <wx/utils.h>

#include <cstdlib>
#include <utility>

CarriedList::CarriedList(wxWindow* parent)
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME) {
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    Bind(wxEVT_LEFT_DOWN, &CarriedList::OnLeftDown, this);
    Bind(wxEVT_MOTION, &CarriedList::OnMotion, this);
    Bind(wxEVT_LEFT_UP, &CarriedList::OnLeftUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &CarriedList::OnCaptureLost, this);
}

void CarriedList::SetLines(std::vector<Line> lines) {
    if (lines == lines_) return;
    lines_ = std::move(lines);
    SetItemCount(lines_.size());
    Refresh();
}

int CarriedList::LineAt(const wxPoint& clientPoint) const {
    const int line = VirtualHitTest(clientPoint.y);
    return line >= 0 && line < static_cast<int>(lines_.size()) ? line : wxNOT_FOUND;
}

wxCoord CarriedList::OnMeasureItem(size_t) const { return GetCharHeight() + FromDIP(4); }

void CarriedList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const {
    if (n >= lines_.size()) return;
    const Line& line = lines_[n];
    const bool selected = IsSelected(n);
    const wxColour grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    const wxColour highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);

    wxRect r = rect;
    r.Deflate(FromDIP(4), 0);
    wxFont font = GetFont();
    dc.SetFont(font);

    if (line.equipped) {
        const wxString tag = line.armour ? "worn" : "readied";
        const wxSize size = dc.GetTextExtent(tag);
        dc.SetTextForeground(selected ? highlight : grey);
        dc.DrawText(tag, r.GetRight() - size.x + 1, r.y + (r.height - size.y) / 2);
        r.width -= size.x + FromDIP(8);
        font.MakeBold();
        dc.SetFont(font);
    }
    dc.SetTextForeground(selected ? highlight
                                  : line.item ? wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT) : grey);
    const wxString text = wxControl::Ellipsize(line.text, dc, wxELLIPSIZE_END, r.width);
    const wxSize size = dc.GetTextExtent(text);
    dc.DrawText(text, r.x, r.y + (r.height - size.y) / 2);
}

void CarriedList::OnLeftDown(wxMouseEvent& event) {
    pressedLine_ = LineAt(event.GetPosition());
    pressedAt_ = event.GetPosition();
    event.Skip();
}

void CarriedList::OnMotion(wxMouseEvent& event) {
    if (dragging_) {
        const bool allowed = onDragOver && onDragOver(ClientToScreen(event.GetPosition()));
        SetCursor(wxCursor(allowed ? wxCURSOR_HAND : wxCURSOR_NO_ENTRY));
        return;
    }
    event.Skip();
    if (!event.LeftIsDown() || pressedLine_ == wxNOT_FOUND) return;
    // Start once the mouse has moved further than a click would.
    const wxPoint moved = event.GetPosition() - pressedAt_;
    if (std::abs(moved.x) < wxSystemSettings::GetMetric(wxSYS_DRAG_X, this) &&
        std::abs(moved.y) < wxSystemSettings::GetMetric(wxSYS_DRAG_Y, this))
        return;
    if (!onDragStart || !onDragStart(pressedLine_)) {
        pressedLine_ = wxNOT_FOUND;
        return;
    }
    dragging_ = true;
    SetSelection(pressedLine_);
    CaptureMouse();
    SetCursor(wxCursor(wxCURSOR_NO_ENTRY));
}

void CarriedList::OnLeftUp(wxMouseEvent& event) {
    pressedLine_ = wxNOT_FOUND;
    if (!dragging_) {
        event.Skip();
        return;
    }
    const wxPoint screen = ClientToScreen(event.GetPosition());
    EndDrag();
    if (onDrop) onDrop(screen);
}

void CarriedList::OnCaptureLost(wxMouseCaptureLostEvent&) {
    dragging_ = false;
    pressedLine_ = wxNOT_FOUND;
    SetCursor(wxNullCursor);
    SetSelection(wxNOT_FOUND);
}

void CarriedList::EndDrag() {
    dragging_ = false;
    if (HasCapture()) ReleaseMouse();
    SetCursor(wxNullCursor);
    SetSelection(wxNOT_FOUND);
}
