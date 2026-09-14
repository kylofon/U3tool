# U3tool

Live party stats and inventory for **Ultima III: Exodus** (the GOG DOS release
running under DOSBox), read straight out of the emulator's memory while you play.

The tool is a native Win32 app in [`u3stats-win32/`](u3stats-win32/), with menus
for editing food and gold and for slowing or pausing the idle turn timer. Build
it with `build.cmd`, then run `U3Stats.exe`.

It finds the party block by signature scan, so it works whatever address DOSBox
happens to allocate, and keeps retrying until the game is running.

The reverse-engineered `PARTY.ULT` / in-memory layout (BCD fields, item tables,
header) is documented in [`u3stats-win32/README.md`](u3stats-win32/README.md#data-layout).

## Requirements

* Windows
* MinGW-w64 (MSYS2 `mingw64`) to build; the exe itself has no dependencies

`u3stats-win32/app.ico` is the game's icon from the GOG install, so keep this
repository private or swap in your own icon before publishing it.
