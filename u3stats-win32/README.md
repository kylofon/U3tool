# Ultima III Assistant — native Win32 build

A live party viewer and editor for Ultima III, built as a standalone Windows executable
using standard Win32 controls: group boxes, static labels,
progress bars for hit points, list boxes for carried items, push buttons and
check boxes, themed through Common Controls v6. No runtime or DLLs beyond
what ships with Windows.

Run `U3Stats.exe`. It attaches to DOSBox by itself and keeps retrying, so start
order doesn't matter. The title bar says **(Connected)** once a party is live and
**(Not connected)** otherwise, with the reason in the status bar.

Each party member's box shows the name with the condition (Good, Poisoned,
Dead, Ashes) to its right, race, class and sex, hit and magic points,
attributes, experience, food, gold, a scrolling **Carrying** list (which marks
the readied weapon and worn armour)
and the gem/key/powder/torch counters. The status bar shows the latest message
on the left and the game speed on the right.

## Menus

* **File → Preferences…** — **Always on top** (on by default).

The main window's position and size, which reference windows are open and
where, and Always on top are remembered between runs in
`%APPDATA%\Ultima III Assistant\settings.ini`. Delete that file to go back to
the defaults. A saved position that no monitor covers any more is ignored.

* **File → Quit**
* **Debug** — its first, greyed line shows the DOSBox process and the party
  block's address.
* **Debug → Rescan memory** — force a fresh search of DOSBox memory.
* **Debug → Next source** — cycle between copies of the party block when more
  than one is found (the item then reads "Next source (*n* of *m*)"); greyed
  out otherwise.
* **Debug → Raw bytes** — shows the annotated 274-byte hex dump below the
  columns; the window grows to make room.
* **Actions → Distribute food** — splits the party's total food evenly across
  every member; any remainder goes one apiece to the first members.
* **Actions → Pool gold → *name*** — moves everyone's gold to that character.
  Nobody can hold more than 9999 (the field is four BCD digits), so anything
  beyond that stays with the others.

* **Cheat → Revive → *name*** — brings a Dead or Ashes character back to Good
  with full hit points (greyed out for anyone alive). In combat the game may
  not put them back on the battlefield until the fight ends.
* **Cheat → Full health → *name*** — sets hit points to their maximum (greyed
  out for the dead and anyone already at full health). Poison isn't cured.
* **Cheat → Cure → *name*** — turns a Poisoned character back to Good (greyed
  out for everyone else).

These actions write straight into DOSBox's memory: they re-read the live party
first and change only the fields they need. The result appears in
the status bar at the bottom; failures also pop up a message box. They're
greyed out unless a party of two or more is live. Changes stick once the game
saves, like anything else that happens in play.

Hit-point bars go yellow at half health and red at a quarter. Status text is
coloured by condition (Good, Poisoned, Dead, Ashes).

## Moving items

Drag an item out of one member's **Carrying** list and drop it anywhere on
another member's column. If the dragged line shows more than one, a pop-up asks
how many to move: use − and +, the mouse wheel, or type a number, then
**Move**. A single item moves straight away.

The same checks run before the pop-up and again against live memory just
before writing:

* Nobody gives away the last of the weapon they have readied or the armour
  they are wearing — change it in game first.
* Nobody carries more than 99 of one item (the count is a single BCD byte).

Like the other actions, the move is written straight into DOSBox's memory
(the recipient is credited first), and the result shows in the status bar.

## Equipping

The readied weapon and worn armour show in **bold** in the Carrying list, tagged
*readied* or *worn*, on a line of their own: two Leather with one worn show as
**Leather × 1** *worn* and Leather × 1. Right-click an item (or press the context-menu key) to
**Ready** / **Wear** it, or to **Put away** / **Take off** the one in use,
which leaves the character with bare hands or skin.

The tool applies the same rules as the game's own Ready and Wear commands,
taken from `EXODUS.BIN`. Dead characters and ashes can't change equipment, you
must own the item, and each class has a limit. Exotic items are allowed for
everyone. When something isn't allowed, the menu item is greyed out with the
reason below it. An equipped item stays counted in the inventory, just as in
the game.

Ultima III's shops unequip a character's weapon or armour whenever they sell
one, whatever the item sold. The assistant notices a sale (equipment dropped,
gold up and that kind of item carried down in the same poll) and puts the
equipment back if the character still owns it. The status bar then says
"Re-equipped after the sale". Selling the last one leaves the character
unequipped. This is skipped when DOSBox could only be opened read-only.

| Class | Weapons up to | Armour up to |
|---|---|---|
| Fighter | any | any |
| Cleric | Mace | Chain |
| Wizard | Dagger | Cloth |
| Thief | Sword | Leather |
| Paladin | any | Plate |
| Barbarian | any | Leather |
| Lark | any | Cloth |
| Illusionist | Mace | Leather |
| Druid | Mace | Cloth |
| Alchemist | Dagger | Cloth |
| Ranger | +2 Swd | +2 Plate |

## Game speed

Ultima III is turn based, except that when you don't press a key for five
seconds it passes a turn for you: monsters close in and food goes down. The
**Game speed** menu changes that wait.

* **Accelerate** / **Slow down**: step the wait through 1, 2, 3, 5 (normal),
  10, 15, 30 and 59 seconds. Either one also ends a pause.
* **Pause**: the game never passes a turn by itself, so nothing moves until
  you act.
* **Normal speed**: back to the game's own five seconds.

The right end of the status bar shows the speed the game is running at, as read
back from memory. You can choose a speed before the game is up and it takes
effect once a party is live. It is re-applied if the game reloads (Alt-R, a new
journey, a DOSBox restart), and the normal wait is put back when the assistant
closes.

How it works: `EXODUS.BIN` has three copies of the same idle-wait loop
(overworld and towns, dungeons, combat). Each sets a deadline of "DOS clock
seconds + 5" and passes a turn when the clock reaches it. The assistant finds them by
signature, rewrites the `5`, and for Pause turns the loop's `jnz` into a `jmp`.
This needs DOSBox's `simple` or `normal` CPU core (the GOG config uses
`simple`). The `dynamic` core caches translated code, so it may miss the change.

## Reference windows

**Reference → Weapons**, **Armour** and **Spells** each open a separate,
resizable window with a table, so you can arrange them around the game and the
assistant. Choosing one that's already open brings it to the front. They follow
the **Always on top** preference.

* **Weapons**: key, kind (mundane, magic, exotic), melee or ranged, price, and
  which classes can ready it.
* **Armour**: key, kind, price, and which classes can wear it.
* **Spells**: wizard and cleric spells grouped, with key, MP cost, effect,
  where they can be cast, what they cure, and which classes can cast them.
  † means only some races of that class; * means all but one race.
  **Castable only** (off by default) hides spells that can't be cast right
  now. In combat, that means by the character whose turn it is; anywhere else,
  by anyone in the party. A spell counts when the character is alive, their
  class has it, they have the MP now, and the current place allows it
  (combat, dungeon, overworld, non-combat or anywhere). The note beside the
  checkbox says whose spells are shown. With no party connected, every spell
  is shown.

The **Key** column is the letter to press in the game: for Ready (weapons), Wear
(armour), or Cast (spells, e.g. Mittar is B, Sanctu is C). The data comes from
the [Gamer Corner Ultima III guide](https://guides.gamercorner.net/ultimaiii/).
Its class columns agree with the limits in `EXODUS.BIN` that the assistant
enforces when equipping. The guide gives no damage or protection numbers, so
none are shown.

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
elevated you'd need to launch this elevated too, and the status bar will say so.

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
| `0x00` | 15 | name, NUL padded |
| `0x0F` | 1 | torches |
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
| `0x20` | 1 | food hundredths, counting down as you travel |
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

Several fields are confirmed from the game's own code in `EXODUS.BIN`:
**Ignite** takes a torch from `0x0F`, **Negate Time** takes a powder from
`0x27`, and eating takes hundredths from `0x20`, borrowing from food at `0x21`.
Gems at `0x25` and keys at `0x26` are still inferred from their position. Buy
one of each in a guild and compare against the in-game Ztats screen to confirm;
if a label is off, swap the `O_GEMS` and `O_KEYS` constants at the top of
`reader.cpp`.

## Building

Needs MinGW-w64 (MSYS2 `mingw64`). Run `build.cmd`; it produces a statically
linked `U3Stats.exe`.

## Files

* `reader.h`, `reader.cpp` — process lookup, memory scan, BCD decoding, and
  the edit and game-speed patches. The field layout is documented above.
* `main.cpp` — window, layout, rendering, polling thread.
* `reference.h`, `reference.cpp` — the Weapons, Armour and Spells reference
  windows and their data.
* `settings.h`, `settings.cpp` — window placement and preferences saved between
  runs.
* `app.rc`, `app.manifest`, `app.ico` — icon, visual styles, DPI awareness,
  version info.
