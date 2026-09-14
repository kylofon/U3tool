# U3Stats — native Win32 build

The same live party viewer as `..\u3stats` (Python), rebuilt as a standalone
Windows executable using standard Win32 controls: group boxes, static labels,
progress bars for hit points, list boxes for carried items, push buttons and
check boxes, themed through Common Controls v6. No runtime or DLLs beyond
what ships with Windows.

Run `U3Stats.exe`. It attaches to DOSBox by itself and keeps retrying, so start
order doesn't matter.

## Controls

* **Rescan** — force a fresh search of DOSBox memory.
* **Next source** — cycle between copies of the party block if more than one
  is found (the status line shows "source *n* of *m*").
* **Always on top** — on by default.
* **Raw bytes** — shows the annotated 274-byte hex dump below the columns; the
  window grows to make room.

## Menus

* **File → Quit**
* **Actions → Distribute food** — splits the party's total food evenly across
  every member; any remainder goes one apiece to the first members.
* **Actions → Pool gold → *name*** — moves everyone's gold to that character.
  Nobody can hold more than 9999 (the field is four BCD digits), so anything
  beyond that stays with the others.

Both actions write straight into DOSBox's memory: they re-read the live party
first and change only the two-byte food or gold fields. The result appears in
the status bar at the bottom; failures also pop up a message box. They're
greyed out unless a party of two or more is live. Changes stick once the game
saves, like anything else that happens in play.

Hit-point bars go yellow at half health and red at a quarter. Status text is
coloured by condition (Good, Poisoned, Dead, Ashes).

## Building

Needs MinGW-w64 (MSYS2 `mingw64`). Run `build.cmd`; it produces a statically
linked `U3Stats.exe`.

## Files

* `reader.h`, `reader.cpp` — process lookup, memory scan, BCD decoding. The
  field layout is documented in `..\u3stats\README.md`.
* `main.cpp` — window, layout, rendering, polling thread.
* `app.rc`, `app.manifest`, `app.ico` — icon, visual styles, DPI awareness,
  version info.
