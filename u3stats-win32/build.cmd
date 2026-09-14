@echo off
rem Builds U3Stats.exe with MinGW-w64 (MSYS2). No other dependencies.
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul || set "PATH=C:\msys64\mingw64\bin;%PATH%"

windres app.rc -O coff -o app.res.o || exit /b 1
g++ -std=c++17 -O2 -s -static -municode -mwindows ^
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DNOMINMAX -Wall ^
    main.cpp reader.cpp app.res.o -lcomctl32 -o U3Stats.exe || exit /b 1
del app.res.o
echo Built %~dp0U3Stats.exe
