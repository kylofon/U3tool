// mainframe.h -- the main window: a box per party member, the menus, the
// status bar and the optional raw-bytes pane. A worker thread polls the game
// through u3::DosBoxReader and hands snapshots to the window.
#pragma once

#include <wx/frame.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "partycolumn.h"
#include "reader.h"

class wxMenu;
class wxMenuEvent;
class wxPanel;
class wxStaticText;
class wxTextCtrl;

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

    // Shows the window where it was last time, and the other windows that were open.
    void RestoreLayout();

    struct Snapshot {
        bool ok = false;
        u3::PartyBytes raw{};
        std::wstring error, exe;
        uint32_t pid = 0;
        uint64_t address = 0;
        size_t candidates = 0, index = 0;
        u3::SpeedState speed;
        int combatTurn = -1;
        bool hasLocation = false;
        u3::Location location;
        std::wstring gameFolder;
    };

private:
    void CreateMenus();
    void Render(const Snapshot& s);
    void RenderSpeed(const Snapshot& s);
    void ShowParty(bool show);
    void SetStatus(int field, const wxString& text);
    void UpdateRaw();
    void ToggleRaw();
    void RequestAction(int action, const wxString& pending);
    void SetSpeed(int step, bool paused);
    void SetTopmost(bool on);
    void SaveLayout();
    void GiveItems(int from, int to, const u3::CarriedItem& item);
    void ShowItemMenu(int member, const wxPoint& screen);
    int DropTarget(const wxPoint& screen) const;
    void RebuildMemberMenus();
    void OnMenuOpen(wxMenuEvent& event);
    void OnClose(wxCloseEvent& event);
#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

    // Worker thread.
    void Work(const std::wstring& host, int port);
    void StopWorker();
    void Wake();

    wxPanel* panel_ = nullptr;
    wxSizer* columnsSizer_ = nullptr;
    wxSizer* waitSizer_ = nullptr;
    PartyColumn columns_[4];
    wxStaticText* wait_ = nullptr;
    wxTextCtrl* raw_ = nullptr;
    wxMenu* actionsMenu_ = nullptr;
    wxMenu* poolMenu_ = nullptr;
    wxMenu* speedMenu_ = nullptr;
    wxMenu* cheatMenu_ = nullptr;
    wxMenu* reviveMenu_ = nullptr;
    wxMenu* healMenu_ = nullptr;
    wxMenu* cureMenu_ = nullptr;
    wxMenu* debugMenu_ = nullptr;
    wxMenuItem* poolItem_ = nullptr;

    bool partyShown_ = true;
    bool showRaw_ = false;
    bool topmost_ = true;
    wxString statusText_[2];
    wxString problem_;  // why we're not connected, as last shown
    wxString debugInfo_ = "Not connected";
    size_t sourceCount_ = 0, sourceIndex_ = 0;
    bool live_ = false;
    bool haveRaw_ = false;
    u3::PartyBytes lastRaw_{};
    wxString lastHex_;
    u3::Party party_;  // last decoded party, for the member menus
    int dragFrom_ = -1;
    u3::CarriedItem dragItem_;

    // Game speed as chosen in the menu, mirrored for the worker.
    int step_;
    bool paused_ = false;
    std::atomic<int> wantSeconds_;
    std::atomic<bool> wantPaused_{false};

    std::atomic<bool> wantRescan_{false}, wantNext_{false};
    std::atomic<int> action_{0};
    std::mutex mutex_;
    std::condition_variable wakeUp_;
    bool stop_ = false, woken_ = false;
    std::thread worker_;
};
