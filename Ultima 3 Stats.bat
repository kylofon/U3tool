@echo off
rem Live party stats for Ultima III. Start the game first (or after) - the
rem window attaches to DOSBox on its own and keeps retrying.
setlocal
cd /d "%~dp0u3stats"
start "" pythonw.exe u3stats.py
if errorlevel 1 start "" python.exe u3stats.py
