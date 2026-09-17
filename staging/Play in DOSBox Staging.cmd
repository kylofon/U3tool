@echo off
rem Starts the GOG Ultima III in DOSBox Staging with its HTTP API turned on,
rem for Ultima III Assistant. Uses the GOG config files unchanged, plus
rem assistant.conf from this folder.
rem
rem Usage: "Play in DOSBox Staging.cmd" [game folder] [path to Staging's dosbox.exe]
setlocal

set "GAME=%~1"
if not defined GAME set "GAME=%ProgramFiles(x86)%\GOG Galaxy\Games\Ultima 3"
if not exist "%GAME%\SOSARIA.ULT" (
    echo Ultima III isn't in "%GAME%". Pass its folder as the first argument.
    pause
    exit /b 1
)

set "STAGING=%~2"
if not defined STAGING if exist "%LOCALAPPDATA%\Programs\DOSBox Staging\dosbox.exe" set "STAGING=%LOCALAPPDATA%\Programs\DOSBox Staging\dosbox.exe"
if not defined STAGING if exist "%ProgramFiles%\DOSBox Staging\dosbox.exe" set "STAGING=%ProgramFiles%\DOSBox Staging\dosbox.exe"
if not defined STAGING (
    echo DOSBox Staging 0.83 or later isn't installed where expected.
    echo Install it from https://www.dosbox-staging.org/ or pass its dosbox.exe as the second argument.
    pause
    exit /b 1
)

rem The GOG config mounts "..", so run from the game's DOSBOX folder as GOG does.
rem The config paths are absolute so the assistant can find the game's folder.
rem The single-player config comes last: its [autoexec] starts the game.
cd /d "%GAME%\DOSBOX"
start "" "%STAGING%" -conf "%GAME%\dosboxULTIMA3.conf" -conf "%~dp0assistant.conf" -conf "%GAME%\dosboxULTIMA3_single.conf" -noconsole -c "exit"
