// settings.h -- settings kept between runs in %APPDATA%\Ultima III Assistant\settings.ini.
#pragma once

#include <windows.h>

namespace settings {

// Remembers a window's normal position and size, and whether it's maximised.
void SaveWindow(const wchar_t* key, HWND hwnd);

// Applies a remembered placement and shows the window. Returns false, leaving
// the window hidden, if nothing usable was saved or it would be off-screen.
bool RestoreWindow(const wchar_t* key, HWND hwnd);

int GetInt(const wchar_t* section, const wchar_t* key, int fallback);
void SetInt(const wchar_t* section, const wchar_t* key, int value);

}  // namespace settings
