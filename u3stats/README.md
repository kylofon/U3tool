# Ultima III — live party stats

A small always-on-top window that shows the current stats and inventory of the
party while you play. It reads the numbers straight out of the running DOSBox
process, so it follows the game live and never touches your save files.

## Running it

Start `Ultima 3 Stats.bat` in the game folder (or `pythonw u3stats/u3stats.py`).
Order doesn't matter — the window attaches to DOSBox on its own and keeps
retrying if the game isn't up yet. The party only exists in memory once you've
picked **Journey Onward**, so the window says "waiting" until then.

Requires Python 3 with tkinter (both already present on this machine). No
third-party packages.

## The window

Four columns, one per party member: name, race/class/sex, status, an HP bar,
magic points, the four attributes, experience, food, gold, what's readied and
worn, everything carried, and the gem/key/powder/torch counters.

* **always on top** — keeps it visible over the DOSBox window.
* **raw bytes** — hex dump of the live 274-byte block, annotated with which
  character record and offset each row belongs to.
* **rescan** — force a fresh search (after loading a different save, say).
* **next source** — if the scan finds more than one copy of the party block,
  this cycles between them. The status bar shows "source *n* of *m*". The tool
  ranks the copy inside DOSBox's big emulated-RAM allocation first, which is
  the live one; a stale copy in a file buffer would be the other.

## How it works

Ultima III keeps the party in RAM in exactly the layout it writes to
`PARTY.ULT`, so `u3reader.py` sweeps DOSBox's committed memory for a byte
pattern that can only be a character record — a printable name, the `0xFF`
in-use marker, a status letter, four BCD attribute bytes, then valid
race/class/sex letters — then checks the 18-byte header in front of it. The
address is cached and re-validated on each 250 ms poll; if it ever stops
looking like a party, the tool rescans by itself.

Reading takes `PROCESS_VM_READ` on DOSBox. Same-user processes are fine; if you
launch the game elevated you'd need to launch this elevated too, and the status
bar will say so.

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
`O_KEYS` and `O_POWDERS` constants at the top of `u3reader.py`.

## Files

* `u3reader.py` — process lookup, memory scan, BCD decoding. Usable on its own.
* `u3stats.py` — the tkinter window.
