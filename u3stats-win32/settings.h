// settings.h -- settings kept between runs in %APPDATA%\Ultima III Assistant\settings.ini.
#pragma once

#include <windows.h>

#include <string>

namespace settings {

// Remembers a window's normal position and size, and whether it's maximised.
void SaveWindow(const wchar_t* key, HWND hwnd);

// Applies a remembered placement and shows the window. Returns false, leaving
// the window hidden, if nothing usable was saved or it would be off-screen.
bool RestoreWindow(const wchar_t* key, HWND hwnd);

int GetInt(const wchar_t* section, const wchar_t* key, int fallback);
void SetInt(const wchar_t* section, const wchar_t* key, int value);

std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);
void SetString(const wchar_t* section, const wchar_t* key, const std::wstring& value);

}  // namespace settings
