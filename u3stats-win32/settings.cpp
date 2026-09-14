// settings.cpp -- settings kept between runs in an INI file under %APPDATA%.
#include "settings.h"

#include <cwchar>
#include <string>

namespace settings {
namespace {

const std::wstring& Path() {
    static const std::wstring path = [] {
        wchar_t appData[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
        const std::wstring dir = (n && n < MAX_PATH) ? std::wstring(appData) + L"\\Ultima III Assistant" : L".";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.ini";
    }();
    return path;
}

}  // namespace

void SaveWindow(const wchar_t* key, HWND hwnd) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof placement;
    if (!GetWindowPlacement(hwnd, &placement)) return;
    const RECT& r = placement.rcNormalPosition;
    wchar_t text[96];
    _snwprintf(text, 95, L"%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom,
               placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    text[95] = 0;
    WritePrivateProfileStringW(L"Windows", key, text, Path().c_str());
}

bool RestoreWindow(const wchar_t* key, HWND hwnd) {
    wchar_t text[96];
    GetPrivateProfileStringW(L"Windows", key, L"", text, 96, Path().c_str());
    RECT r{};
    int maximized = 0;
    if (std::swscanf(text, L"%ld,%ld,%ld,%ld,%d", &r.left, &r.top, &r.right, &r.bottom, &maximized) != 5) return false;
    // Skip nonsense sizes, and places no monitor covers any more.
    if (r.right - r.left < 100 || r.bottom - r.top < 60 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false;

    WINDOWPLACEMENT placement{};
    placement.length = sizeof placement;
    GetWindowPlacement(hwnd, &placement);
    placement.flags = 0;
    placement.rcNormalPosition = r;
    placement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    return SetWindowPlacement(hwnd, &placement) != 0;
}

int GetInt(const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, Path().c_str()));
}

void SetInt(const wchar_t* section, const wchar_t* key, int value) {
    WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), Path().c_str());
}

}  // namespace settings
