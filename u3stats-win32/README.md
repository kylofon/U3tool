# U3Stats — native Win32 build

A live party viewer for Ultima III, built as a standalone Windows executable
using standard Win32 controls: group boxes, static labels,
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

## Game speed

Ultima III is turn based, except that when you don't press a key for five
seconds it passes a turn for you: monsters close in and food goes down. The
**Game speed** menu changes that wait.

* **Accelerate** / **Slow down**: step the wait through 1, 2, 3, 5 (normal),
  10, 15, 30 and 59 seconds. Either one also ends a pause.
* **Pause**: the game never passes a turn by itself, so nothing moves until
  you act.
* **Normal speed**: back to the game's own five seconds.

The line under the status text shows the speed the game is running at, as read
back from memory. You can choose a speed before the game is up and it takes
effect once a party is live. It is re-applied if the game reloads (Alt-R, a new
journey, a DOSBox restart), and the normal wait is put back when U3Stats closes.

How it works: `EXODUS.BIN` has three copies of the same idle-wait loop
(overworld and towns, dungeons, combat). Each sets a deadline of "DOS clock
seconds + 5" and passes a turn when the clock reaches it. U3Stats finds them by
signature, rewrites the `5`, and for Pause turns the loop's `jnz` into a `jmp`.
This needs DOSBox's `simple` or `normal` CPU core (the GOG config uses
`simple`). The `dynamic` core caches translated code, so it may miss the change.

## How it works

Ultima III keeps the party in RAM in exactly the layout it writes to
`PARTY.ULT`, so the reader sweeps DOSBox's committed memory for a byte pattern
that can only be a character record — a printable name, the `0xFF` in-use
marker, a status letter, four BCD attribute bytes, then valid race/class/sex
letters — then checks the 18-byte header in front of it. The address is cached
and re-validated on each 250 ms poll; if it ever stops looking like a party,
the tool rescans by itself. Copies inside DOSBox's big emulated-RAM allocation
rank first, since that is the live one; a stale copy in a file buffer would be
the other source.

Reading takes `PROCESS_VM_READ` on DOSBox, and the edit actions also need
`PROCESS_VM_WRITE`. Same-user processes are fine; if you launch the game
elevated you'd need to launch this elevated too, and the status line will say so.

## Data layout

274 bytes: an 18-byte header followed by four 64-byte character records.
**Every number is packed BCD, little endian** — two bytes are four decimal
digits, so `50 01` is 150, not 336.

### Header

| Offset | Meaning |
|---|---|
| `0x07` | party size (1–4) |
| `0x0A`–`0x0D` | roster slot of each member |

Other header bytes hold map position and travel state; they're visible in the
raw pane but not decoded.

### Character record

| Offset | Size | Field |
|---|---|---|
| `0x00` | 16 | name, NUL padded |
| `0x10` | 1 | `0xFF` = slot in use |
| `0x11` | 1 | status — `G`ood, `P`oisoned, `D`ead, `A`shes |
| `0x12`–`0x15` | 4 | strength, dexterity, intelligence, wisdom |
| `0x16` | 1 | race — `HEDBFN` |
| `0x17` | 1 | class — `FCWTPLBDIAR` |
| `0x18` | 1 | sex — `MFO` |
| `0x19` | 1 | magic points |
| `0x1A` | 2 | hit points |
| `0x1C` | 2 | max hit points |
| `0x1E` | 2 | experience |
| `0x20` | 1 | torches |
| `0x21` | 2 | food |
| `0x23` | 2 | gold |
| `0x25` | 1 | gems |
| `0x26` | 1 | keys |
| `0x27` | 1 | powders |
| `0x28` | 1 | armour worn (index) |
| `0x29` | 7 | armour carried, types 1–7 |
| `0x30` | 1 | weapon readied (index) |
| `0x31` | 15 | weapons carried, types 1–15 |

The name tables come out of `BOOTUP.BIN`:

* Weapons 0–15: Hand, Dagger, Mace, Sling, Axe, Bow, Sword, 2-H-Swd, +2 Axe,
  +2 Bow, +2 Swd, Gloves, +4 Axe, +4 Bow, +4 Swd, Exotic
* Armour 0–7: Skin, Cloth, Leather, Chain, Plate, +2 Chain, +2 Plate, Exotic

Index 0 of each list ("Hand", "Skin") means nothing readied or worn, which is
why the carried arrays start at type 1 and fill the record exactly to 64 bytes.

### Note on the four counters

`0x21` (food), `0x23` (gold) and everything above are pinned down by the save
data. The single-byte counters at `0x20` and `0x25`–`0x27` are inferred: in the
save used to work this out all four characters had 15 at `0x20` and 0 at
`0x25`–`0x27`, so the *torches / gems / keys / powders* labels are a best guess
at the ordering. Buy one of each in a guild and compare against the in-game
Ztats screen to confirm; if a label is off, swap the `O_TORCHES`, `O_GEMS`,
`O_KEYS` and `O_POWDERS` constants at the top of `reader.cpp`.

## Building

Needs MinGW-w64 (MSYS2 `mingw64`). Run `build.cmd`; it produces a statically
linked `U3Stats.exe`.

## Files

* `reader.h`, `reader.cpp` — process lookup, memory scan, BCD decoding, and
  the edit and game-speed patches. The field layout is documented above.
* `main.cpp` — window, layout, rendering, polling thread.
* `app.rc`, `app.manifest`, `app.ico` — icon, visual styles, DPI awareness,
  version info.
