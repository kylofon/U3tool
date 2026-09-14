"""
u3reader.py -- find and decode the live Ultima III party block inside a
running DOSBox process.

Ultima III keeps the party in RAM in exactly the layout it writes to
PARTY.ULT, so we can locate it by signature and then just re-read 274 bytes
whenever we want a fresh view:

    offset  size  meaning
    0x00      18  party header
    0x12      64  character record 0
    0x52      64  character record 1
    0x92      64  character record 2
    0xD2      64  character record 3

Numbers are packed BCD, little endian (two bytes == four decimal digits),
which is why 0x50 0x01 reads as 150 rather than 336.
"""

from __future__ import annotations

import ctypes
import re
from ctypes import wintypes
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# Win32 plumbing
# --------------------------------------------------------------------------

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPPROCESS = 0x00000002
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
READABLE_PROTECT = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__alignment1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__alignment2", wintypes.DWORD),
    ]


k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.OpenProcess.restype = wintypes.HANDLE
k32.CloseHandle.argtypes = [wintypes.HANDLE]
k32.CloseHandle.restype = wintypes.BOOL
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32FirstW.restype = wintypes.BOOL
k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.restype = wintypes.BOOL
k32.VirtualQueryEx.argtypes = [
    wintypes.HANDLE,
    ctypes.c_ulonglong,
    ctypes.POINTER(MEMORY_BASIC_INFORMATION64),
    ctypes.c_size_t,
]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE,
    ctypes.c_ulonglong,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
k32.ReadProcessMemory.restype = wintypes.BOOL


def find_processes(*name_fragments):
    """Return (pid, exe name) for every process whose name contains a fragment."""
    wanted = tuple(f.lower() for f in name_fragments)
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wintypes.HANDLE(-1).value:
        return []
    found = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = k32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            low = entry.szExeFile.lower()
            if any(w in low for w in wanted):
                found.append((entry.th32ProcessID, entry.szExeFile))
            ok = k32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snap)
    return found


# --------------------------------------------------------------------------
# Ultima III data tables (lifted straight out of BOOTUP.BIN)
# --------------------------------------------------------------------------

WEAPONS = [
    "Hand", "Dagger", "Mace", "Sling", "Axe", "Bow", "Sword", "2-H-Swd",
    "+2 Axe", "+2 Bow", "+2 Swd", "Gloves", "+4 Axe", "+4 Bow", "+4 Swd",
    "Exotic",
]
ARMOUR = [
    "Skin", "Cloth", "Leather", "Chain", "Plate", "+2 Chain", "+2 Plate",
    "Exotic",
]
RACES = {"H": "Human", "E": "Elf", "D": "Dwarf", "B": "Bobbit", "F": "Fuzzy",
         "N": "Other"}
CLASSES = {"F": "Fighter", "C": "Cleric", "W": "Wizard", "T": "Thief",
           "P": "Paladin", "L": "Lark", "B": "Barbarian", "D": "Druid",
           "I": "Illusionist", "A": "Alchemist", "R": "Ranger"}
SEXES = {"M": "Male", "F": "Female", "O": "Other"}
STATUSES = {"G": "Good", "P": "Poisoned", "D": "Dead", "A": "Ashes"}

RECORD_SIZE = 64
HEADER_SIZE = 18
PARTY_SIZE = HEADER_SIZE + 4 * RECORD_SIZE  # 274

# Character record field offsets.
O_NAME, NAME_LEN = 0x00, 16
O_INUSE = 0x10
O_STATUS = 0x11
O_STR, O_DEX, O_INT, O_WIS = 0x12, 0x13, 0x14, 0x15
O_RACE, O_CLASS, O_SEX = 0x16, 0x17, 0x18
O_MP = 0x19
O_HP, O_MAXHP, O_EXP = 0x1A, 0x1C, 0x1E
O_TORCHES = 0x20
O_FOOD = 0x21
O_GOLD = 0x23
O_GEMS, O_KEYS, O_POWDERS = 0x25, 0x26, 0x27
O_ARMOUR_WORN, O_ARMOUR_INV = 0x28, 0x29   # worn index, then armour 1..7
O_WEAPON_READY, O_WEAPON_INV = 0x30, 0x31  # readied index, then weapons 1..15

# Party header offsets.
H_COUNT = 0x07
H_SLOTS = 0x0A  # four roster slot numbers, one per party member


def bcd1(b):
    hi, lo = b >> 4, b & 0x0F
    return None if hi > 9 or lo > 9 else hi * 10 + lo


def bcd2(buf, off):
    lo, hi = bcd1(buf[off]), bcd1(buf[off + 1])
    return None if lo is None or hi is None else hi * 100 + lo


# --------------------------------------------------------------------------
# Decoded views
# --------------------------------------------------------------------------


@dataclass
class Character:
    raw: bytes
    name: str = ""
    status: str = ""
    status_code: str = ""
    race: str = ""
    klass: str = ""
    sex: str = ""
    strength: object = None
    dexterity: object = None
    intelligence: object = None
    wisdom: object = None
    hp: object = None
    max_hp: object = None
    mp: object = None
    exp: object = None
    food: object = None
    gold: object = None
    gems: object = None
    keys: object = None
    powders: object = None
    torches: object = None
    weapon: str = ""
    armour: str = ""
    weapons_owned: list = field(default_factory=list)
    armour_owned: list = field(default_factory=list)

    @property
    def present(self):
        return bool(self.name) or self.raw[O_INUSE] == 0xFF


def decode_character(raw):
    c = Character(raw=raw)
    c.name = raw[O_NAME:O_NAME + NAME_LEN].split(b"\x00")[0].decode(
        "cp437", "replace").strip()
    c.status_code = chr(raw[O_STATUS]) if 32 <= raw[O_STATUS] < 127 else "?"
    c.status = STATUSES.get(c.status_code, "? (0x%02x)" % raw[O_STATUS])
    c.race = RACES.get(chr(raw[O_RACE]), "?0x%02x" % raw[O_RACE])
    c.klass = CLASSES.get(chr(raw[O_CLASS]), "?0x%02x" % raw[O_CLASS])
    c.sex = SEXES.get(chr(raw[O_SEX]), "?0x%02x" % raw[O_SEX])

    c.strength = bcd1(raw[O_STR])
    c.dexterity = bcd1(raw[O_DEX])
    c.intelligence = bcd1(raw[O_INT])
    c.wisdom = bcd1(raw[O_WIS])
    c.mp = bcd1(raw[O_MP])
    c.hp = bcd2(raw, O_HP)
    c.max_hp = bcd2(raw, O_MAXHP)
    c.exp = bcd2(raw, O_EXP)
    c.food = bcd2(raw, O_FOOD)
    c.gold = bcd2(raw, O_GOLD)
    c.gems = bcd1(raw[O_GEMS])
    c.keys = bcd1(raw[O_KEYS])
    c.powders = bcd1(raw[O_POWDERS])
    c.torches = bcd1(raw[O_TORCHES])

    w = raw[O_WEAPON_READY]
    a = raw[O_ARMOUR_WORN]
    c.weapon = WEAPONS[w] if w < len(WEAPONS) else "?0x%02x" % w
    c.armour = ARMOUR[a] if a < len(ARMOUR) else "?0x%02x" % a

    for i in range(1, 16):
        n = bcd1(raw[O_WEAPON_INV + i - 1]) or 0
        if n:
            c.weapons_owned.append((WEAPONS[i], n))
    for i in range(1, 8):
        n = bcd1(raw[O_ARMOUR_INV + i - 1]) or 0
        if n:
            c.armour_owned.append((ARMOUR[i], n))
    return c


@dataclass
class Party:
    raw: bytes
    count: int
    slots: list
    characters: list


def decode_party(raw):
    count = raw[H_COUNT]
    slots = list(raw[H_SLOTS:H_SLOTS + 4])
    chars = [
        decode_character(raw[HEADER_SIZE + i * RECORD_SIZE:
                             HEADER_SIZE + (i + 1) * RECORD_SIZE])
        for i in range(4)
    ]
    return Party(raw=raw, count=count, slots=slots, characters=chars)


# --------------------------------------------------------------------------
# Locating the party block
# --------------------------------------------------------------------------

# One character record: a printable name, the 0xFF in-use marker, a known
# status letter, four BCD stat bytes, then race/class/sex letters.
RECORD_SIG = re.compile(
    rb"[\x21-\x7E][\x20-\x7E\x00]{15}"
    rb"\xFF[GPDA]"
    rb"[\x00-\x99]{4}"
    rb"[HEDBFN][FCWTPLBDIAR][MFO]",
    re.DOTALL,
)


def looks_like_record(raw):
    if len(raw) < RECORD_SIZE or raw[O_INUSE] != 0xFF:
        return False
    return (chr(raw[O_STATUS]) in STATUSES
            and chr(raw[O_RACE]) in RACES
            and chr(raw[O_CLASS]) in CLASSES
            and chr(raw[O_SEX]) in SEXES)


def looks_like_party(raw):
    """Cheap sanity check, used to rank candidates and to detect staleness."""
    if len(raw) != PARTY_SIZE:
        return False
    count = raw[H_COUNT]
    if not 1 <= count <= 4:
        return False
    slots = list(raw[H_SLOTS:H_SLOTS + count])
    if len(set(slots)) != count or any(not 0 <= s <= 31 for s in slots):
        return False
    for i in range(count):
        rec = raw[HEADER_SIZE + i * RECORD_SIZE:
                  HEADER_SIZE + (i + 1) * RECORD_SIZE]
        if not looks_like_record(rec):
            return False
    return True


class DosBoxReader:
    """Holds a handle on DOSBox and remembers where the party block lives."""

    def __init__(self, process_names=("dosbox",)):
        self.process_names = process_names
        self.pid = None
        self.exe = ""
        self.handle = None
        self.candidates = []
        self.index = 0
        self.last_error = ""

    @property
    def address(self):
        if 0 <= self.index < len(self.candidates):
            return self.candidates[self.index]
        return None

    def close(self):
        if self.handle:
            k32.CloseHandle(self.handle)
        self.handle = None
        self.pid = None
        self.exe = ""
        self.candidates = []
        self.index = 0

    def attach(self):
        procs = find_processes(*self.process_names)
        if not procs:
            self.close()
            self.last_error = "DOSBox is not running."
            return False
        if self.handle and any(self.pid == p for p, _ in procs):
            return True
        self.close()
        for pid, exe in procs:
            h = k32.OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
            if h:
                self.handle, self.pid, self.exe = h, pid, exe
                self.last_error = ""
                return True
        # Found DOSBox but could not open it -- almost always elevation.
        self.pid, self.exe = procs[0]
        self.last_error = ("Cannot open %s (pid %d) -- try running this tool "
                           "as administrator." % (self.exe, self.pid))
        return False

    def read(self, address, size):
        if not self.handle:
            return None
        buf = (ctypes.c_char * size)()
        got = ctypes.c_size_t(0)
        ok = k32.ReadProcessMemory(
            self.handle, address, buf, size, ctypes.byref(got))
        if not ok or got.value != size:
            return None
        return bytes(buf)

    def regions(self):
        mbi = MEMORY_BASIC_INFORMATION64()
        addr = 0
        limit = 0x7FFFFFFFFFFF
        while addr < limit:
            if not k32.VirtualQueryEx(
                    self.handle, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            size = mbi.RegionSize
            if size == 0:
                break
            if (mbi.State == MEM_COMMIT
                    and (mbi.Protect & READABLE_PROTECT)
                    and not (mbi.Protect & PAGE_GUARD)):
                yield mbi.BaseAddress, size
            addr = mbi.BaseAddress + size

    def scan(self):
        """Full sweep of DOSBox's address space for party blocks."""
        if not self.handle:
            return []
        hits = []  # (region size, party base address)
        chunk = 4 << 20
        overlap = PARTY_SIZE + RECORD_SIZE * 4

        for base, size in self.regions():
            if size < 0x10000 or size > (1 << 30):
                continue
            pos = 0
            while pos < size:
                n = min(chunk, size - pos)
                buf = self.read(base + pos, n)
                if buf:
                    for m in RECORD_SIG.finditer(buf):
                        rec_addr = base + pos + m.start()
                        # The match may be any of the four records.
                        for k in range(4):
                            party = rec_addr - HEADER_SIZE - k * RECORD_SIZE
                            block = self.read(party, PARTY_SIZE)
                            if block and looks_like_party(block):
                                hits.append((size, party))
                                break
                if n < chunk:
                    break
                pos += chunk - overlap

        # Deduplicate, then prefer the biggest region -- DOSBox's emulated RAM
        # is one large allocation, while stray copies sit in small heap blocks.
        seen = {}
        for region_size, party in hits:
            seen[party] = max(seen.get(party, 0), region_size)
        self.candidates = [a for a, _ in
                           sorted(seen.items(), key=lambda kv: -kv[1])]
        self.index = 0
        return self.candidates

    def next_candidate(self):
        if self.candidates:
            self.index = (self.index + 1) % len(self.candidates)

    def poll(self):
        """Read the party block, rescanning if our cached address went bad."""
        if not self.attach():
            return None
        addr = self.address
        if addr is not None:
            raw = self.read(addr, PARTY_SIZE)
            if raw and looks_like_party(raw):
                return decode_party(raw)
        if not self.scan():
            self.last_error = ("Attached to %s, but no party block found yet "
                               "-- load a saved game first." % self.exe)
            return None
        addr = self.address
        if addr is None:
            return None
        raw = self.read(addr, PARTY_SIZE)
        if raw and looks_like_party(raw):
            return decode_party(raw)
        return None
