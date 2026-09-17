@echo off
rem Builds U3Stats.exe with MinGW-w64 (MSYS2). No other dependencies.
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul || set "PATH=C:\msys64\mingw64\bin;%PATH%"

windres app.rc -O coff -o app.res.o || exit /b 1
g++ -std=c++17 -O2 -s -static -municode -mwindows ^
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DNOMINMAX -Wall -I..\core ^
    main.cpp reference.cpp maps.cpp settings.cpp crash.cpp ^
    ..\core\reader.cpp ..\core\staging.cpp ..\core\platform_win32.cpp ..\core\mapdata.cpp ..\core\refdata.cpp app.res.o -lcomctl32 -ldbghelp -liphlpapi -lws2_32 -lntdll ^
    -Wl,-Map,U3Stats.map -o U3Stats.exe || exit /b 1
del app.res.o
echo Built %~dp0U3Stats.exe
