# U3tool

Live party stats and inventory for **Ultima III: Exodus** (the GOG DOS release
running under DOSBox), read straight out of the emulator's memory while you play.

Two implementations of the same viewer:

| Folder | What | Run |
|---|---|---|
| [`u3stats-win32/`](u3stats-win32/) | Native Win32 app with menus for editing food and gold | `build.cmd`, then `U3Stats.exe` |
| [`u3stats/`](u3stats/) | Original Python/tkinter viewer, read-only | `Ultima 3 Stats.bat` or `pythonw u3stats/u3stats.py` |

Both find the party block by signature scan, so they work whatever address
DOSBox happens to allocate, and both keep retrying until the game is running.

The reverse-engineered `PARTY.ULT` / in-memory layout (BCD fields, item tables,
header) is documented in [`u3stats/README.md`](u3stats/README.md).

## Requirements

* Windows
* Native app: MinGW-w64 (MSYS2 `mingw64`) to build; the exe itself has no dependencies
* Python viewer: Python 3 with tkinter

`u3stats-win32/app.ico` is the game's icon from the GOG install, so keep this
repository private or swap in your own icon before publishing it.
