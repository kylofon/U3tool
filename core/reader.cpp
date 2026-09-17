// reader.cpp -- choosing the emulator, memory scan, BCD decoding and edits.
#include "reader.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace u3 {
namespace {

// Name tables, lifted straight out of BOOTUP.BIN.
const wchar_t* const WEAPONS[16] = {
    L"Hand",   L"Dagger", L"Mace",   L"Sling",  L"Axe",    L"Bow",    L"Sword",  L"2-H-Swd",
    L"+2 Axe", L"+2 Bow", L"+2 Swd", L"Gloves", L"+4 Axe", L"+4 Bow", L"+4 Swd", L"Exotic",
};
const wchar_t* const ARMOUR[8] = {
    L"Skin", L"Cloth", L"Leather", L"Chain", L"Plate", L"+2 Chain", L"+2 Plate", L"Exotic",
};

struct Code {
    char code;
    const wchar_t* name;
};
const Code RACES[] = {{'H', L"Human"}, {'E', L"Elf"},   {'D', L"Dwarf"},
                      {'B', L"Bobbit"}, {'F', L"Fuzzy"}, {'N', L"Other"}};
const Code CLASSES[] = {{'F', L"Fighter"},     {'C', L"Cleric"},    {'W', L"Wizard"},
                        {'T', L"Thief"},       {'P', L"Paladin"},   {'L', L"Lark"},
                        {'B', L"Barbarian"},   {'D', L"Druid"},     {'I', L"Illusionist"},
                        {'A', L"Alchemist"},   {'R', L"Ranger"}};
const Code SEXES[] = {{'M', L"Male"}, {'F', L"Female"}, {'O', L"Other"}};
const Code STATUSES[] = {{'G', L"Good"}, {'P', L"Poisoned"}, {'D', L"Dead"}, {'A', L"Ashes"}};

// Character record field offsets.
constexpr size_t O_NAME = 0x00, NAME_LEN = 14;
constexpr size_t O_MARKS = 0x0E;       // marks and cards, one flag each
constexpr uint8_t MARK_OF_KINGS = 0x80;  // Lord British wants this past 500 max HP
constexpr size_t O_TORCHES = 0x0F;  // Ignite takes one from here
constexpr size_t O_INUSE = 0x10, O_STATUS = 0x11;
constexpr size_t O_STR = 0x12, O_DEX = 0x13, O_INT = 0x14, O_WIS = 0x15;
constexpr size_t O_RACE = 0x16, O_CLASS = 0x17, O_SEX = 0x18;
constexpr size_t O_MP = 0x19, O_HP = 0x1A, O_MAXHP = 0x1C, O_EXP = 0x1E;
constexpr size_t O_FOOD = 0x21, O_GOLD = 0x23;  // 0x20 holds food hundredths, used up as you travel
constexpr size_t O_GEMS = 0x25, O_KEYS = 0x26, O_POWDERS = 0x27;
constexpr size_t O_ARMOUR_WORN = 0x28, O_ARMOUR_INV = 0x29;    // worn index, then armour 1..7
constexpr size_t O_WEAPON_READY = 0x30, O_WEAPON_INV = 0x31;   // readied index, then weapons 1..15

// Party header offsets.
constexpr size_t H_MAP = 0x02;
constexpr size_t H_ENTRY_X = 0x08, H_ENTRY_Y = 0x09;  // Sosaria position saved on entering somewhere
constexpr size_t H_COUNT = 0x07;
constexpr size_t H_SLOTS = 0x0A;

template <size_t N>
const wchar_t* Find(const Code (&table)[N], uint8_t b) {
    for (const Code& c : table)
        if (static_cast<uint8_t>(c.code) == b) return c.name;
    return nullptr;
}

std::wstring Unknown(uint8_t b) {
    static const wchar_t HEX[] = L"0123456789ABCDEF";
    std::wstring s = L"?0x";
    s += HEX[b >> 4];
    s += HEX[b & 0x0F];
    return s;
}

std::wstring NameOr(const wchar_t* name, uint8_t b) { return name ? std::wstring(name) : Unknown(b); }

int Bcd1(uint8_t b) {
    int hi = b >> 4, lo = b & 0x0F;
    return (hi > 9 || lo > 9) ? -1 : hi * 10 + lo;
}

int Bcd2(const uint8_t* r, size_t off) {
    int lo = Bcd1(r[off]), hi = Bcd1(r[off + 1]);
    return (lo < 0 || hi < 0) ? -1 : hi * 100 + lo;
}

uint8_t ToBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

const uint8_t* Record(const PartyBytes& raw, int member) {
    return raw.data() + HEADER_SIZE + member * RECORD_SIZE;
}

// The idle wait, identical in the overworld/town, dungeon and combat input
// loops of EXODUS.BIN:
//
//       mov ah,2Ch / int 21h      ; DOS time, DH = seconds
//       mov bl,dh / add bl,05     ; deadline = now + 5 s
//       cmp bl,3Ch / jb +3 / sub bl,3Ch
//   loop:
//       call check_keystroke / jnz <read the key>
//       mov ah,2Ch / int 21h
//       cmp bl,dh / jnz loop      ; not there yet
//       ...no key, so pass a turn
//
// The add's immediate sets the wait; turning that jnz into a jmp waits forever.
constexpr uint8_t IDLE_SETUP[] = {0xB4, 0x2C, 0xCD, 0x21, 0x8A, 0xDE, 0x80, 0xC3, 0x05,
                                  0x80, 0xFB, 0x3C, 0x72, 0x03, 0x80, 0xEB, 0x3C};
constexpr size_t IDLE_SECONDS = 8;                // offset of the add's immediate
constexpr size_t IDLE_LOOP = sizeof IDLE_SETUP;   // the loop starts right after the setup
constexpr size_t IDLE_WINDOW = IDLE_LOOP + 0x40;  // its closing jump lies within this
constexpr uint8_t OP_JNZ = 0x75, OP_JMP = 0xEB;

// Offsets in EXODUS.BIN's 64K segment: the party block, and the overworld
// idle wait (the first of the three).
constexpr uint64_t EXODUS_PARTY_AT = 0x14BA, EXODUS_IDLE_AT = 0x1B82;
constexpr size_t EXODUS_SEGMENT = 0x10000;

// If p (IDLE_WINDOW bytes) is an idle wait, the offset of its jnz/jmp opcode; else 0.
size_t MatchIdleLoop(const uint8_t* p) {
    for (size_t i = 0; i < IDLE_LOOP; ++i)
        if (i != IDLE_SECONDS && p[i] != IDLE_SETUP[i]) return 0;
    for (size_t j = IDLE_LOOP; j + 4 <= IDLE_WINDOW; ++j) {
        if (p[j] != 0x3A || p[j + 1] != 0xDE || (p[j + 2] != OP_JNZ && p[j + 2] != OP_JMP)) continue;
        const ptrdiff_t target = static_cast<ptrdiff_t>(j + 4) + static_cast<int8_t>(p[j + 3]);
        if (target == static_cast<ptrdiff_t>(IDLE_LOOP)) return j + 2;
    }
    return 0;
}

bool LooksLikeRecord(const uint8_t* r) {
    return r[O_INUSE] == 0xFF && Find(STATUSES, r[O_STATUS]) && Find(RACES, r[O_RACE]) &&
           Find(CLASSES, r[O_CLASS]) && Find(SEXES, r[O_SEX]);
}

// A printable name, the 0xFF in-use marker, a status letter, four BCD
// attribute bytes, then race/class/sex letters. Needs 0x19 readable bytes.
bool MatchesSignature(const uint8_t* p) {
    if (p[O_INUSE] != 0xFF || p[0] < 0x21 || p[0] > 0x7E) return false;
    for (size_t i = 1; i < NAME_LEN; ++i)
        if (p[i] != 0 && (p[i] < 0x20 || p[i] > 0x7E)) return false;
    for (size_t i = O_STR; i <= O_WIS; ++i)
        if (p[i] > 0x99) return false;
    return LooksLikeRecord(p);
}

Character DecodeCharacter(const uint8_t* r) {
    Character c;
    for (size_t i = 0; i < NAME_LEN && r[O_NAME + i]; ++i) {
        uint8_t b = r[O_NAME + i];
        c.name += (b >= 0x20 && b < 0x7F) ? static_cast<wchar_t>(b) : L'?';
    }
    size_t first = c.name.find_first_not_of(L' ');
    c.name = first == std::wstring::npos ? L"" : c.name.substr(first, c.name.find_last_not_of(L' ') - first + 1);
    c.present = !c.name.empty() || r[O_INUSE] == 0xFF;

    c.statusCode = static_cast<char>(r[O_STATUS]);
    c.classCode = static_cast<char>(r[O_CLASS]);
    c.status = NameOr(Find(STATUSES, r[O_STATUS]), r[O_STATUS]);
    c.race = NameOr(Find(RACES, r[O_RACE]), r[O_RACE]);
    c.klass = NameOr(Find(CLASSES, r[O_CLASS]), r[O_CLASS]);
    c.sex = NameOr(Find(SEXES, r[O_SEX]), r[O_SEX]);

    c.strength = Bcd1(r[O_STR]);
    c.dexterity = Bcd1(r[O_DEX]);
    c.intelligence = Bcd1(r[O_INT]);
    c.wisdom = Bcd1(r[O_WIS]);
    c.mp = Bcd1(r[O_MP]);
    c.hp = Bcd2(r, O_HP);
    c.maxHp = Bcd2(r, O_MAXHP);
    c.exp = Bcd2(r, O_EXP);

    // Ztats shows the level as the hundreds of experience, plus one.
    const int expHundreds = Bcd1(r[O_EXP + 1]), hpHundreds = Bcd1(r[O_MAXHP + 1]);
    c.level = expHundreds < 0 ? -1 : std::min(expHundreds + 1, 99);
    // Lord British adds 100 max HP while its hundreds don't exceed experience's,
    // up to 2500, and from 500 on only for someone with the Mark of Kings.
    c.canLevelUp = expHundreds >= 0 && hpHundreds >= 0 && hpHundreds <= expHundreds && hpHundreds < 25 &&
                   (hpHundreds < 5 || (r[O_MARKS] & MARK_OF_KINGS)) && (r[O_STATUS] == 'G' || r[O_STATUS] == 'P');
    c.food = Bcd2(r, O_FOOD);
    c.gold = Bcd2(r, O_GOLD);
    c.gems = Bcd1(r[O_GEMS]);
    c.keys = Bcd1(r[O_KEYS]);
    c.powders = Bcd1(r[O_POWDERS]);
    c.torches = Bcd1(r[O_TORCHES]);

    uint8_t w = r[O_WEAPON_READY], a = r[O_ARMOUR_WORN];
    c.weapon = w < 16 ? std::wstring(WEAPONS[w]) : Unknown(w);
    c.armour = a < 8 ? std::wstring(ARMOUR[a]) : Unknown(a);

    // The item in use gets a line of its own, apart from any spares of its type.
    auto carry = [&c](const wchar_t* name, int n, bool armour, size_t type, bool equipped) {
        if (n <= 0) return;
        if (equipped) {
            c.carried.push_back({name, 1, armour, static_cast<int>(type), true});
            if (--n == 0) return;
        }
        c.carried.push_back({name, n, armour, static_cast<int>(type), false});
    };
    for (size_t i = 1; i < 16; ++i) carry(WEAPONS[i], Bcd1(r[O_WEAPON_INV + i - 1]), false, i, w == i);
    for (size_t i = 1; i < 8; ++i) carry(ARMOUR[i], Bcd1(r[O_ARMOUR_INV + i - 1]), true, i, a == i);
    return c;
}

bool ValidItem(bool armour, int type) { return type >= 1 && type < (armour ? 8 : 16); }
const wchar_t* ItemName(bool armour, int type) { return armour ? ARMOUR[type] : WEAPONS[type]; }
size_t InventoryOffset(bool armour, int type) { return (armour ? O_ARMOUR_INV : O_WEAPON_INV) + type - 1; }

// Class limits used by EXODUS.BIN's Ready and Wear commands: the class letters
// (at 7A05) and, for each, the letter of the first weapon and armour it may
// not use (79EF, 79FA), counting 'A' as Hand/Skin. Exotic items are exempt.
constexpr char EQUIP_CLASSES[] = "FCWTPBLIDAR";
constexpr char WEAPON_LIMITS[] = "QDCHQQQDDCL";
constexpr char ARMOUR_LIMITS[] = "IECDFDCDCCH";
constexpr int EXOTIC_WEAPON = 15, EXOTIC_ARMOUR = 7;

// What to say when an edit's write didn't go in. `partWay` if an earlier
// write of the same edit did; `what` names what to check in game then.
std::wstring NotWritten(WriteResult why, bool partWay, const std::wstring& what) {
    if (why == WriteResult::Changed)
        return partWay ? L"The game changed " + what + L" part-way through — check " + what + L" in game."
                       : L"The game changed " + what + L" in the meantime — nothing was changed. Try again.";
    return partWay ? L"Writing to DOSBox failed part-way — check " + what + L" in game."
                   : L"Writing to DOSBox failed — nothing was changed.";
}

}  // namespace

bool LooksLikeParty(const uint8_t* raw) {
    int count = raw[H_COUNT];
    if (count < 1 || count > 4) return false;
    for (int i = 0; i < count; ++i) {
        uint8_t slot = raw[H_SLOTS + i];
        if (slot > 31) return false;
        for (int j = 0; j < i; ++j)
            if (raw[H_SLOTS + j] == slot) return false;
        if (!LooksLikeRecord(raw + HEADER_SIZE + i * RECORD_SIZE)) return false;
    }
    return true;
}

Party DecodeParty(const uint8_t* raw) {
    Party p;
    p.count = raw[H_COUNT];
    p.map = raw[H_MAP];
    for (int i = 0; i < 4; ++i) {
        p.slots[i] = raw[H_SLOTS + i];
        p.chars[i] = DecodeCharacter(raw + HEADER_SIZE + i * RECORD_SIZE);
    }
    return p;
}

int MovableItems(const uint8_t* raw, const ItemMove& move, std::wstring* why) {
    auto none = [why](std::wstring reason) {
        if (why) *why = std::move(reason);
        return 0;
    };
    const int count = raw[H_COUNT];
    if (move.from < 0 || move.from >= count || move.to < 0 || move.to >= count || move.from == move.to)
        return none(L"Those two aren't both in the party any more.");
    if (!ValidItem(move.armour, move.type)) return none(L"That isn't an item Ultima III knows.");

    const uint8_t* giver = raw + HEADER_SIZE + move.from * RECORD_SIZE;
    const uint8_t* taker = raw + HEADER_SIZE + move.to * RECORD_SIZE;
    const std::wstring item = ItemName(move.armour, move.type);
    const size_t at = InventoryOffset(move.armour, move.type);
    const int have = Bcd1(giver[at]), held = Bcd1(taker[at]);
    if (have < 0 || held < 0) return none(L"An item count isn't valid BCD — nothing was changed.");
    if (have == 0) return none(DecodeCharacter(giver).name + L" has no " + item + L" any more.");

    const bool equipped = giver[move.armour ? O_ARMOUR_WORN : O_WEAPON_READY] == move.type;
    const int spare = have - (equipped ? 1 : 0);
    if (spare <= 0)
        return none(DecodeCharacter(giver).name +
                    (move.armour ? L" is wearing their only " + item + L" — take it off in game first."
                                 : L" has their only " + item + L" readied — ready something else in game first."));
    const int room = MAX_BCD1 - held;
    if (room <= 0)
        return none(DecodeCharacter(taker).name + L" already carries " + std::to_wstring(MAX_BCD1) + L" " + item +
                    L", the most one character can hold.");
    return std::min(spare, room);
}

std::wstring EquipProblem(const uint8_t* raw, const Equip& equip) {
    if (equip.member < 0 || equip.member >= raw[H_COUNT]) return L"That party member isn't in the party any more.";
    const uint8_t* r = raw + HEADER_SIZE + equip.member * RECORD_SIZE;
    const Character ch = DecodeCharacter(r);
    if (r[O_STATUS] == 'D' || r[O_STATUS] == 'A') return ch.name + L" is " + ch.status + L" and can't change equipment.";
    if (equip.type == 0) return L"";
    if (!ValidItem(equip.armour, equip.type)) return L"That isn't an item Ultima III knows.";

    const std::wstring item = ItemName(equip.armour, equip.type);
    if (Bcd1(r[InventoryOffset(equip.armour, equip.type)]) <= 0) return ch.name + L" has no " + item + L".";
    if (equip.type == (equip.armour ? EXOTIC_ARMOUR : EXOTIC_WEAPON)) return L"";

    int limit = 0;  // an unknown class may use nothing
    for (int i = 0; EQUIP_CLASSES[i]; ++i)
        if (static_cast<uint8_t>(EQUIP_CLASSES[i]) == r[O_CLASS])
            limit = (equip.armour ? ARMOUR_LIMITS : WEAPON_LIMITS)[i] - 'A';
    if (equip.type >= limit)
        return L"Not allowed: " + ch.name + L" (" + ch.klass + L") can't " + (equip.armour ? L"wear " : L"ready ") +
               item + L".";
    return L"";
}

DosBoxReader::DosBoxReader(const std::wstring& stagingHost, int stagingPort)
    : staging_(ConnectStaging(stagingHost, stagingPort)), process_(ConnectProcess()) {}

void DosBoxReader::Reset() {
    active_ = nullptr;
    session_ = 0;
    pid = 0;
    canWrite = false;
    exe.clear();
    idleLoops_.clear();
    idleParty_ = 0;
    lastIdleScan_ = {};
    candidates.clear();
    index = 0;
}

uint64_t DosBoxReader::Address() const { return index < candidates.size() ? candidates[index] : 0; }

uint64_t DosBoxReader::Session() const { return session_; }

bool DosBoxReader::Attach() {
    // Prefer Staging's API whenever it answers: Staging is a "dosbox" process
    // too, but the API is the better way in. Reading the process is the fallback.
    Emulator* found = nullptr;
    if (staging_->Connect())
        found = staging_.get();
    else if (process_ && process_->Connect())
        found = process_.get();

    if (!found) {
        Reset();
        if (!process_)
            lastError = staging_->error + L" Is DOSBox Staging running with webserver_enabled = on?";
        else if (process_->error == L"DOSBox is not running.")
            lastError = L"DOSBox is not running (for DOSBox Staging, turn on webserver_enabled).";
        else
            lastError = process_->error;
        return false;
    }
    if (found != active_ || found->session != session_) {
        Reset();
        active_ = found;
        session_ = found->session;
    }
    pid = found->pid;
    exe = found->name;
    canWrite = found->CanWrite();
    lastError.clear();
    return true;
}

bool DosBoxReader::Read(uint64_t address, uint8_t* buffer, size_t size) const {
    return active_ && active_->Read(address, buffer, size);
}

void DosBoxReader::Scan() {
    candidates.clear();
    index = 0;
    if (!active_) return;

    std::map<uint64_t, uint64_t> found;  // party address -> size of the region holding it
    PartyBytes block;
    active_->ForEachChunk([&](uint64_t at, const uint8_t* buf, size_t n, uint64_t regionSize) {
        for (size_t i = 0; i + 0x19 <= n; ++i) {
            if (buf[i + O_INUSE] != 0xFF || !MatchesSignature(&buf[i])) continue;
            uint64_t record = at + i;
            // The match may be any of the four records.
            for (size_t k = 0; k < 4; ++k) {
                uint64_t back = HEADER_SIZE + k * RECORD_SIZE;
                if (record < back) break;
                uint64_t party = record - back;
                if (Read(party, block.data(), PARTY_SIZE) && LooksLikeParty(block.data())) {
                    found[party] = std::max(found[party], regionSize);
                    break;
                }
            }
        }
    });

    // The live party has the running game's code beside it; a stale copy of the
    // file in one of DOSBox's buffers usually doesn't. Region size only breaks
    // ties, since those buffers can outgrow the emulated RAM.
    struct Ranked {
        uint64_t address, regionSize;
        bool beside;
    };
    std::vector<Ranked> ranked;
    uint8_t window[IDLE_WINDOW];
    for (const auto& f : found) {
        const bool beside = f.first >= EXODUS_PARTY_AT &&
                            Read(f.first - EXODUS_PARTY_AT + EXODUS_IDLE_AT, window, IDLE_WINDOW) &&
                            MatchIdleLoop(window) != 0;
        ranked.push_back({f.first, f.second, beside});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.beside != b.beside ? a.beside : a.regionSize > b.regionSize;
    });
    for (const Ranked& r : ranked) candidates.push_back(r.address);
}

int DosBoxReader::CombatTurn() const {
    // EXODUS.BIN keeps the party block at 14BA in its segment and the combat
    // turn at 84E1, so the turn sits a fixed distance past the party.
    constexpr uint64_t PARTY_AT = 0x14BA, TURN_AT = 0x84E1;
    uint8_t turn = 0xFF;
    if (index >= candidates.size() || !Read(candidates[index] + (TURN_AT - PARTY_AT), &turn, 1) || turn > 3)
        return -1;
    return turn;
}

bool DosBoxReader::ReadLocation(const PartyBytes& raw, Location& out) const {
    // In EXODUS.BIN's segment: party block at 14BA, then x, y, torch and dungeon
    // level at 15CC, and the dungeon facing at 58CC.
    constexpr uint64_t PARTY_AT = 0x14BA, POSITION_AT = 0x15CC, FACING_AT = 0x58CC;
    if (index >= candidates.size()) return false;
    const uint64_t party = candidates[index];
    uint8_t position[4], facing = 0;
    if (!Read(party + (POSITION_AT - PARTY_AT), position, sizeof position) ||
        !Read(party + (FACING_AT - PARTY_AT), &facing, 1))
        return false;
    out.live = true;
    out.map = raw[H_MAP];
    out.entryX = raw[H_ENTRY_X];
    out.entryY = raw[H_ENTRY_Y];
    out.x = position[0] & 0x3F;
    out.y = position[1] & 0x3F;
    out.torch = position[2];
    out.level = position[3] & 0x07;
    out.facing = facing & 0x03;
    return true;
}

std::wstring DosBoxReader::GameFolder() const { return active_ ? FindGameFolder(active_->pid) : L""; }

void DosBoxReader::NextCandidate() {
    if (!candidates.empty()) index = (index + 1) % candidates.size();
}

bool DosBoxReader::Poll(PartyBytes& out) {
    if (!Attach()) return false;
    if (index < candidates.size() && Read(candidates[index], out.data(), PARTY_SIZE) &&
        LooksLikeParty(out.data())) {
        lastError.clear();
        return true;
    }
    Scan();
    if (candidates.empty()) {
        lastError = L"Attached to " + exe + L", but no party block found yet — load a saved game first.";
        return false;
    }
    if (Read(candidates[index], out.data(), PARTY_SIZE) && LooksLikeParty(out.data())) {
        lastError.clear();
        return true;
    }
    return false;
}

bool DosBoxReader::ReadLive(PartyBytes& out) const {
    return index < candidates.size() && Read(candidates[index], out.data(), PARTY_SIZE) &&
           LooksLikeParty(out.data());
}

bool DosBoxReader::BeginAction(PartyBytes& raw, ActionResult& result) const {
    if (!ReadLive(raw)) {
        result.message = L"No party in memory — load a saved game first.";
        return false;
    }
    if (!canWrite) {
        result.message = L"DOSBox could only be opened read-only — run Ultima III Assistant as administrator to change values.";
        return false;
    }
    return true;
}

WriteResult DosBoxReader::Write(uint64_t address, const uint8_t* bytes, size_t size, const uint8_t* expected) {
    return active_ && canWrite ? active_->Write(address, bytes, size, expected) : WriteResult::Failed;
}

WriteResult DosBoxReader::WriteField(const PartyBytes& raw, int member, size_t offset, const uint8_t* bytes,
                                     size_t size) {
    const size_t at = HEADER_SIZE + member * RECORD_SIZE + offset;
    return Write(candidates[index] + at, bytes, size, raw.data() + at);
}

WriteResult DosBoxReader::WriteBcd2(const PartyBytes& raw, int member, size_t offset, int value) {
    const uint8_t bytes[2] = {ToBcd(value % 100), ToBcd(value / 100 % 100)};
    return WriteField(raw, member, offset, bytes, sizeof bytes);
}

ActionResult DosBoxReader::DistributeFood() {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;

    const int count = raw[H_COUNT];
    int food[4] = {}, total = 0;
    for (int i = 0; i < count; ++i) {
        food[i] = Bcd2(Record(raw, i), O_FOOD);
        if (food[i] < 0) {
            result.message = L"A food value isn't valid BCD — nothing was changed.";
            return result;
        }
        total += food[i];
    }

    // An even split, with any remainder going one apiece to the first members.
    const int share = total / count, extra = total % count;
    bool partWay = false;
    for (int i = 0; i < count; ++i) {
        const int value = share + (i < extra ? 1 : 0);
        if (value == food[i]) continue;
        const WriteResult written = WriteBcd2(raw, i, O_FOOD, value);
        if (written != WriteResult::Ok) {
            result.message = NotWritten(written, partWay, L"food");
            return result;
        }
        partWay = true;
    }

    result.ok = true;
    result.message = L"Distributed " + std::to_wstring(total) + L" food: " + std::to_wstring(share) + L" each";
    if (extra) result.message += L", plus 1 for the first " + std::to_wstring(extra);
    result.message += L".";
    return result;
}

ActionResult DosBoxReader::PoolGold(int target) {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;

    const int count = raw[H_COUNT];
    if (target < 0 || target >= count) {
        result.message = L"That party member is no longer in the party.";
        return result;
    }

    int gold[4] = {}, before[4] = {};
    for (int i = 0; i < count; ++i) {
        gold[i] = before[i] = Bcd2(Record(raw, i), O_GOLD);
        if (gold[i] < 0) {
            result.message = L"A gold value isn't valid BCD — nothing was changed.";
            return result;
        }
    }

    const int room = MAX_BCD2 - gold[target];
    int moved = 0;
    for (int i = 0; i < count && moved < room; ++i) {
        if (i == target) continue;
        const int take = std::min(gold[i], room - moved);
        gold[i] -= take;
        moved += take;
    }
    gold[target] += moved;

    // Credit the recipient first: if a later write fails, gold is duplicated
    // rather than lost.
    bool partWay = false;
    for (int step = 0; step <= count; ++step) {
        const int i = step == 0 ? target : step - 1;
        if ((step > 0 && i == target) || gold[i] == before[i]) continue;
        const WriteResult written = WriteBcd2(raw, i, O_GOLD, gold[i]);
        if (written != WriteResult::Ok) {
            result.message = NotWritten(written, partWay, L"gold");
            return result;
        }
        partWay = true;
    }

    int leftOver = 0;
    for (int i = 0; i < count; ++i)
        if (i != target) leftOver += gold[i];

    result.ok = true;
    result.message = L"Moved " + std::to_wstring(moved) + L" gold to " + DecodeCharacter(Record(raw, target)).name +
                     L" (now " + std::to_wstring(gold[target]) + L").";
    if (leftOver)
        result.message += L" " + std::to_wstring(leftOver) + L" stayed with the others — " +
                          std::to_wstring(MAX_BCD2) + L" is the most one character can carry.";
    return result;
}

ActionResult DosBoxReader::Revive(int member) {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;
    if (member < 0 || member >= raw[H_COUNT]) {
        result.message = L"That party member is no longer in the party.";
        return result;
    }

    const uint8_t* r = Record(raw, member);
    const std::wstring name = DecodeCharacter(r).name;
    if (r[O_STATUS] != 'D' && r[O_STATUS] != 'A') {
        result.message = name + L" isn't dead.";
        return result;
    }
    const int maxHp = Bcd2(r, O_MAXHP);
    if (maxHp <= 0) {
        result.message = name + L"'s maximum hit points aren't valid BCD — nothing was changed.";
        return result;
    }

    // Hit points first, so nobody is ever alive with none.
    const uint8_t good = 'G';
    WriteResult written = WriteBcd2(raw, member, O_HP, maxHp);
    const bool partWay = written == WriteResult::Ok;
    if (partWay) written = WriteField(raw, member, O_STATUS, &good, 1);
    if (written != WriteResult::Ok) {
        result.message = NotWritten(written, partWay, name);
        return result;
    }
    result.ok = true;
    result.message = name + L" is alive again, with full health.";
    return result;
}

ActionResult DosBoxReader::FullHealth(int member) {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;
    if (member < 0 || member >= raw[H_COUNT]) {
        result.message = L"That party member is no longer in the party.";
        return result;
    }

    const uint8_t* r = Record(raw, member);
    const std::wstring name = DecodeCharacter(r).name;
    if (r[O_STATUS] == 'D' || r[O_STATUS] == 'A') {
        result.message = name + L" is dead — revive them first.";
        return result;
    }
    const int hp = Bcd2(r, O_HP), maxHp = Bcd2(r, O_MAXHP);
    if (hp < 0 || maxHp <= 0) {
        result.message = name + L"'s hit points aren't valid BCD — nothing was changed.";
        return result;
    }

    if (hp >= maxHp) {
        result.ok = true;
        result.message = name + L" already has full health.";
        return result;
    }
    const WriteResult written = WriteBcd2(raw, member, O_HP, maxHp);
    result.ok = written == WriteResult::Ok;
    result.message = result.ok ? name + L" is back to full health (" + std::to_wstring(maxHp) + L" HP)."
                               : NotWritten(written, false, name + L"'s hit points");
    return result;
}

ActionResult DosBoxReader::Cure(int member) {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;
    if (member < 0 || member >= raw[H_COUNT]) {
        result.message = L"That party member is no longer in the party.";
        return result;
    }

    const uint8_t* r = Record(raw, member);
    const std::wstring name = DecodeCharacter(r).name;
    if (r[O_STATUS] != 'P') {
        result.message = name + L" isn't poisoned.";
        return result;
    }
    const uint8_t good = 'G';
    const WriteResult written = WriteField(raw, member, O_STATUS, &good, 1);
    if (written != WriteResult::Ok) {
        result.message = NotWritten(written, false, name);
        return result;
    }
    result.ok = true;
    result.message = name + L" is cured of poison.";
    return result;
}

ActionResult DosBoxReader::MoveItems(const ItemMove& move) {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;

    const int movable = MovableItems(raw.data(), move, &result.message);
    if (movable == 0) return result;
    if (move.count < 1 || move.count > movable) {
        result.message = L"The inventories changed in the meantime — nothing was moved.";
        return result;
    }

    const size_t at = InventoryOffset(move.armour, move.type);
    const uint8_t given = ToBcd(Bcd1(Record(raw, move.to)[at]) + move.count);
    const uint8_t kept = ToBcd(Bcd1(Record(raw, move.from)[at]) - move.count);

    // Credit the recipient first: if the second write fails, items are
    // duplicated rather than lost.
    WriteResult written = WriteField(raw, move.to, at, &given, 1);
    const bool partWay = written == WriteResult::Ok;
    if (partWay) written = WriteField(raw, move.from, at, &kept, 1);
    if (written != WriteResult::Ok) {
        result.message = NotWritten(written, partWay, L"the inventories");
        return result;
    }

    result.ok = true;
    result.message = L"Moved " + std::to_wstring(move.count) + L" × " + ItemName(move.armour, move.type) +
                     L" from " + DecodeCharacter(Record(raw, move.from)).name + L" to " +
                     DecodeCharacter(Record(raw, move.to)).name + L".";
    return result;
}

std::vector<Equip> EquipmentLostToSale(const uint8_t* before, const uint8_t* after) {
    std::vector<Equip> lost;
    const int count = after[H_COUNT];
    if (before[H_COUNT] != count || std::memcmp(before + H_SLOTS, after + H_SLOTS, 4) != 0) return lost;

    for (int member = 0; member < count; ++member) {
        const uint8_t* was = before + HEADER_SIZE + member * RECORD_SIZE;
        const uint8_t* now = after + HEADER_SIZE + member * RECORD_SIZE;
        if (std::memcmp(was, now, NAME_LEN) != 0) continue;
        const int goldBefore = Bcd2(was, O_GOLD), goldAfter = Bcd2(now, O_GOLD);
        if (goldBefore < 0 || goldAfter <= goldBefore) continue;  // a sale pays

        for (bool armour : {false, true}) {
            const size_t field = armour ? O_ARMOUR_WORN : O_WEAPON_READY;
            const int type = was[field];
            if (type == 0 || now[field] != 0 || !ValidItem(armour, type)) continue;

            // Something of this kind must have gone, and some of the equipped
            // item must be left; selling the last one leaves it unequipped.
            const int kinds = armour ? 7 : 15;
            const size_t inventory = armour ? O_ARMOUR_INV : O_WEAPON_INV;
            int carriedBefore = 0, carriedAfter = 0;
            for (int i = 0; i < kinds; ++i) {
                carriedBefore += std::max(Bcd1(was[inventory + i]), 0);
                carriedAfter += std::max(Bcd1(now[inventory + i]), 0);
            }
            if (carriedAfter < carriedBefore && Bcd1(now[InventoryOffset(armour, type)]) > 0)
                lost.push_back({member, armour, type});
        }
    }
    return lost;
}

ActionResult DosBoxReader::SetEquipped(const Equip& equip) {
    ActionResult result;
    PartyBytes raw;
    if (!BeginAction(raw, result)) return result;

    result.message = EquipProblem(raw.data(), equip);
    if (!result.message.empty()) return result;

    // Like the game, store the type index; the item stays in the inventory count.
    const uint8_t type = static_cast<uint8_t>(equip.type);
    const size_t field = equip.armour ? O_ARMOUR_WORN : O_WEAPON_READY;
    const WriteResult written = WriteField(raw, equip.member, field, &type, 1);
    if (written != WriteResult::Ok) {
        result.message = NotWritten(written, false, L"the equipment");
        return result;
    }

    const std::wstring name = DecodeCharacter(Record(raw, equip.member)).name;
    result.ok = true;
    if (equip.type == 0)
        result.message = name + (equip.armour ? L" took their armour off." : L" put their weapon away.");
    else
        result.message = name + (equip.armour ? L" is now wearing " : L" readied ") + ItemName(equip.armour, equip.type) +
                         L".";
    return result;
}

void DosBoxReader::ScanIdleLoops() {
    idleLoops_.clear();
    idleParty_ = 0;
    if (!active_ || index >= candidates.size() || candidates[index] < EXODUS_PARTY_AT) return;

    // The loops that run are in the same copy of EXODUS.BIN as the live party:
    // the 64K segment around it. DOSBox can hold stale copies of the file in
    // buffers, even bigger ones than its emulated RAM, so look nowhere else.
    const uint64_t segment = candidates[index] - EXODUS_PARTY_AT;
    std::vector<uint8_t> code(EXODUS_SEGMENT);
    if (!Read(segment, code.data(), code.size())) return;
    for (size_t i = 0; i + IDLE_WINDOW <= code.size(); ++i) {
        if (code[i] != IDLE_SETUP[0] || code[i + 1] != IDLE_SETUP[1]) continue;
        if (size_t jumpAt = MatchIdleLoop(&code[i])) idleLoops_.push_back({segment + i, jumpAt});
    }
    idleParty_ = candidates[index];
}

SpeedState DosBoxReader::SyncSpeed(const GameSpeed& want, bool allowScan) {
    SpeedState state;
    if (!active_) return state;

    // The loops belong to the live party's copy of the game; if the party is
    // now read from elsewhere, find them again.
    if (!idleLoops_.empty() && (index >= candidates.size() || candidates[index] != idleParty_)) {
        idleLoops_.clear();
        lastIdleScan_ = {};
    }

    // The game reloads EXODUS.BIN on Alt-R, a new journey and so on, so make
    // sure the loops are still where we found them.
    uint8_t window[IDLE_WINDOW];
    for (const IdleLoop& loop : idleLoops_) {
        if (!Read(loop.address, window, IDLE_WINDOW) || MatchIdleLoop(window) != loop.jumpAt) {
            idleLoops_.clear();
            lastIdleScan_ = {};  // they moved: look again straight away
            break;
        }
    }
    if (idleLoops_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (!allowScan || now - lastIdleScan_ < std::chrono::seconds(3)) return state;
        lastIdleScan_ = now;
        ScanIdleLoops();
        if (idleLoops_.empty()) return state;
    }

    const uint8_t seconds = static_cast<uint8_t>(std::min(std::max(want.passSeconds, 1), MAX_PASS_SECONDS));
    const uint8_t jump = want.paused ? OP_JMP : OP_JNZ;
    state.sites = idleLoops_.size();
    state.applied = true;
    for (const IdleLoop& loop : idleLoops_) {
        const bool ok = Read(loop.address, window, IDLE_WINDOW) &&
                        (window[IDLE_SECONDS] == seconds ||
                         Write(loop.address + IDLE_SECONDS, &seconds, 1, &window[IDLE_SECONDS]) == WriteResult::Ok) &&
                        (window[loop.jumpAt] == jump ||
                         Write(loop.address + loop.jumpAt, &jump, 1, &window[loop.jumpAt]) == WriteResult::Ok);
        if (!ok) state.applied = false;
    }

    // Report what the game is really running with now.
    const IdleLoop& first = idleLoops_.front();
    if (Read(first.address, window, IDLE_WINDOW)) {
        state.actual.passSeconds = window[IDLE_SECONDS];
        state.actual.paused = window[first.jumpAt] == OP_JMP;
    }
    if (!state.applied)
        state.error = canWrite ? L"writing to DOSBox failed"
                               : L"DOSBox is read-only — run Ultima III Assistant as administrator to change it";
    return state;
}

}  // namespace u3
