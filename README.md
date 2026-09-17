# U3tool

Live party stats and inventory for **Ultima III: Exodus** (the GOG DOS release
running under DOSBox classic or DOSBox Staging), read straight out of the emulator's memory while you play.
It prefers [DOSBox Staging](https://www.dosbox-staging.org/)'s HTTP API and
falls back to reading the DOSBox process's memory directly.

Features:
- Main window shows details about each party member, including their inventories.
	- EXP indicator highlights when level up is available from Lord British.
	- Drag and drop interface allows you to move items between characters.
	- Wear and ready items directly from your inventory; equipment a shop unequips on a sale is put back.
	- Pool gold or distribute food evenly between your characters.
	- Cheat menu for healing, curing and resurrecting.
	- Game speed menu to shorten, lengthen or pause the wait before the game passes a turn by itself.
- Map window for displaying world and dungeon maps.
	- Indicates player's current position with a blinky cursor.
	- Allows browsing other maps than the one you're currently in.
	- Traces dungeons during discovery or discovers the whole map; exploration progress can be rolled back.
- Reference windows with spell, weapon and armor information.
	- Display full information about items and spells.
	- Spell list can dynamically limit spells to show only those castable by current active character.
	- Item lists show which character can use given item when hovering over that item's row.
- Open windows and their positions are remembered between runs.

**Ultima III Assistant** is a native Win32 app in [`u3stats-win32/`](u3stats-win32/). Build
it with `u3stats-win32/build.cmd`, then run `U3Stats.exe`. Its README covers
every menu and window.

It finds the party block by signature scan, so it works whatever address DOSBox
happens to allocate, and keeps retrying until the game is running.

To play in DOSBox Staging 0.83+ with its API on, run
[`staging/Play in DOSBox Staging.cmd`](staging/). It uses the GOG install's
config files unchanged, plus `staging/assistant.conf`. See
[Connecting to the game](u3stats-win32/README.md#connecting-to-the-game).

The UI is being moved to a cross-platform toolkit. Everything that isn't UI
(reading and editing the game, the reference and map data) already lives in
[`core/`](core/), which has no Windows dependencies apart from the optional
direct process-memory access.

The reverse-engineered `PARTY.ULT` / in-memory layout (BCD fields, item tables,
header) is documented in [`u3stats-win32/README.md`](u3stats-win32/README.md#data-layout).

## Requirements

* Windows (the core also builds on Linux)
* DOSBox Staging 0.83 or later is recommended; GOG's bundled DOSBox 0.74 works too
* MinGW-w64 (MSYS2 `mingw64`) to build; the exe itself has no dependencies
* [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.56.0 (MIT), vendored in
  `core/third_party/cpp-httplib`

GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml))
builds every push: the core on Linux, linked into a small smoke check that it
runs, and the Windows app with MinGW. The built `U3Stats.exe` is attached to
each run for 14 days.

`u3stats-win32/app.ico` is the game's icon from the GOG install, so keep this
repository private or swap in your own icon before publishing it.
