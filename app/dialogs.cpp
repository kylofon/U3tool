// dialogs.cpp -- the About box, Preferences and the "move how many" prompt.
#include "dialogs.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valnum.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#include <shellapi.h>
#endif

#include <algorithm>

#include "icon.h"
#include "version.h"

const char* const APP_TITLE = "Ultima III Assistant";

namespace {

const char* const WEBSITE = "https://kkania.com";
const char* const SOURCE = "https://github.com/kylofon/ultima3-assistant";
const char* const SUPPORT = "https://buymeacoffee.com/krzysztofkania";

#ifdef __WXMSW__
HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}
#else
// A label and a link on one line, for the portable About box.
void AddLink(wxWindow* parent, wxSizer* sizer, const wxString& label, const wxString& text, const wxString& url) {
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new wxStaticText(parent, wxID_ANY, label + " "), 0, wxALIGN_CENTER_VERTICAL);
    line->Add(new wxHyperlinkCtrl(parent, wxID_ANY, text, url), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line);
}
#endif

}  // namespace

void ShowAbout(wxWindow* parent) {
    const wxString title = wxString("About ") + APP_TITLE;
    const wxString heading = wxString(APP_TITLE) + " " + APP_VERSION_TEXT;
#ifdef __WXMSW__
    // The Windows task dialog, as the Win32 build shows it.
    const wxString content = wxString::Format(
        "A live party viewer and editor for Ultima III: Exodus.\n\n"
        "Author: Krzysztof Kania\n"
        "Website: <a href=\"%s\">kkania.com</a>\n"
        "Source: <a href=\"%s\">github.com/kylofon/ultima3-assistant</a>\n"
        "Support: <a href=\"%s\">buymeacoffee.com/krzysztofkania</a>",
        WEBSITE, SOURCE, SUPPORT);
    wxIcon icon;
    icon.CopyFromBitmap(AppBitmap(32));
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = static_cast<HWND>(parent->GetHWND());
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = title.wc_str();
    dialog.hMainIcon = static_cast<HICON>(icon.GetHICON());
    dialog.pszMainInstruction = heading.wc_str();
    dialog.pszContent = content.wc_str();
    dialog.pfCallback = AboutCallback;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
#else
    wxDialog dialog(parent, wxID_ANY, title);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(new wxStaticBitmap(&dialog, wxID_ANY, AppBitmap(dialog.FromDIP(48))), 0, wxALL, dialog.FromDIP(12));

    auto* text = new wxBoxSizer(wxVERTICAL);
    auto* headingText = new wxStaticText(&dialog, wxID_ANY, heading);
    headingText->SetFont(dialog.GetFont().Bold().Scaled(1.3f));
    text->Add(headingText, 0, wxBOTTOM, dialog.FromDIP(8));
    text->Add(new wxStaticText(&dialog, wxID_ANY, "A live party viewer and editor for Ultima III: Exodus."), 0,
              wxBOTTOM, dialog.FromDIP(12));
    text->Add(new wxStaticText(&dialog, wxID_ANY, "Author: Krzysztof Kania"));
    AddLink(&dialog, text, "Website:", "kkania.com", WEBSITE);
    AddLink(&dialog, text, "Source:", "github.com/kylofon/ultima3-assistant", SOURCE);
    AddLink(&dialog, text, "Support:", "buymeacoffee.com/krzysztofkania", SUPPORT);
    body->Add(text, 1, wxTOP | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(body, 1, wxEXPAND);
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, dialog.FromDIP(8));
    dialog.SetSizerAndFit(all);
    dialog.CentreOnParent();
    dialog.ShowModal();
#endif
}

bool ShowPreferences(wxWindow* parent, bool& alwaysOnTop) {
    wxDialog dialog(parent, wxID_ANY, "Preferences");
    auto* all = new wxBoxSizer(wxVERTICAL);
    auto* topmost = new wxCheckBox(&dialog, wxID_ANY, "Always on &top");
    topmost->SetValue(alwaysOnTop);
    all->Add(topmost, 0, wxALL, dialog.FromDIP(12));
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
             dialog.FromDIP(12));
    dialog.SetSizerAndFit(all);
    dialog.SetMinSize(wxSize(dialog.FromDIP(300), -1));
    dialog.Fit();
    dialog.CentreOnParent();
    topmost->SetFocus();
    if (dialog.ShowModal() != wxID_OK || topmost->GetValue() == alwaysOnTop) return false;
    alwaysOnTop = topmost->GetValue();
    return true;
}

int AskCount(wxWindow* parent, const wxString& prompt, int max) {
    wxDialog dialog(parent, wxID_ANY, "Move items");
    int value = max;
    const int pad = dialog.FromDIP(12);

    auto* all = new wxBoxSizer(wxVERTICAL);
    auto* text = new wxStaticText(&dialog, wxID_ANY, prompt);
    text->Wrap(dialog.FromDIP(316));
    all->Add(text, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    const wxSize stepSize(dialog.FromDIP(28), dialog.FromDIP(26));
    auto* minus = new wxButton(&dialog, wxID_ANY, wxString::FromUTF8("−"), wxDefaultPosition, stepSize);
    auto* count = new wxTextCtrl(&dialog, wxID_ANY, "", wxDefaultPosition, wxSize(dialog.FromDIP(56), -1),
                                 wxTE_CENTRE);
    count->SetMaxLength(2);
    count->SetValidator(wxIntegerValidator<int>());
    auto* plus = new wxButton(&dialog, wxID_ANY, "+", wxDefaultPosition, stepSize);
    row->Add(minus, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(count, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, dialog.FromDIP(4));
    row->Add(plus, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(new wxStaticText(&dialog, wxID_ANY, wxString::Format("of %d", max)), 0,
             wxALIGN_CENTER_VERTICAL | wxLEFT, dialog.FromDIP(10));
    all->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    auto* buttons = new wxStdDialogButtonSizer;
    auto* move = new wxButton(&dialog, wxID_OK, "Move");
    move->SetDefault();
    buttons->AddButton(move);
    buttons->AddButton(new wxButton(&dialog, wxID_CANCEL));
    buttons->Realize();
    all->Add(buttons, 0, wxEXPAND | wxALL, pad);

    auto show = [&](int v) {
        value = std::min(std::max(v, 1), max);
        count->ChangeValue(wxString::Format("%d", value));
    };
    // Track what's typed, but only tidy the text once focus leaves.
    count->Bind(wxEVT_TEXT, [&](wxCommandEvent&) {
        long typed = 0;
        count->GetValue().ToLong(&typed);
        value = std::min(std::max(static_cast<int>(typed), 1), max);
    });
    count->Bind(wxEVT_KILL_FOCUS, [&](wxFocusEvent& event) {
        show(value);
        event.Skip();
    });
    minus->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { show(value - 1); });
    plus->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { show(value + 1); });
    auto wheel = [&](wxMouseEvent& event) { show(value + (event.GetWheelRotation() > 0 ? 1 : -1)); };
    for (wxWindow* w : {static_cast<wxWindow*>(&dialog), static_cast<wxWindow*>(count),
                        static_cast<wxWindow*>(minus), static_cast<wxWindow*>(plus)})
        w->Bind(wxEVT_MOUSEWHEEL, wheel);

    dialog.SetSizerAndFit(all);
    dialog.SetSize(wxSize(std::max(dialog.GetSize().x, dialog.FromDIP(340)), -1));
    dialog.CentreOnParent();
    show(max);
    count->SetFocus();
    count->SelectAll();
    return dialog.ShowModal() == wxID_OK ? value : 0;
}
