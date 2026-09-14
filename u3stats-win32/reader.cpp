// reader.cpp -- process lookup, memory scan and BCD decoding.
#include "reader.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
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
constexpr size_t O_NAME = 0x00, NAME_LEN = 16;
constexpr size_t O_INUSE = 0x10, O_STATUS = 0x11;
constexpr size_t O_STR = 0x12, O_DEX = 0x13, O_INT = 0x14, O_WIS = 0x15;
constexpr size_t O_RACE = 0x16, O_CLASS = 0x17, O_SEX = 0x18;
constexpr size_t O_MP = 0x19, O_HP = 0x1A, O_MAXHP = 0x1C, O_EXP = 0x1E;
constexpr size_t O_TORCHES = 0x20, O_FOOD = 0x21, O_GOLD = 0x23;
constexpr size_t O_GEMS = 0x25, O_KEYS = 0x26, O_POWDERS = 0x27;
constexpr size_t O_ARMOUR_WORN = 0x28, O_ARMOUR_INV = 0x29;    // worn index, then armour 1..7
constexpr size_t O_WEAPON_READY = 0x30, O_WEAPON_INV = 0x31;   // readied index, then weapons 1..15

// Party header offsets.
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

// Two BCD bytes hold four digits, so no character can carry more than this.
constexpr int MAX_BCD2 = 9999;

const uint8_t* Record(const PartyBytes& raw, int member) {
    return raw.data() + HEADER_SIZE + member * RECORD_SIZE;
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
    c.food = Bcd2(r, O_FOOD);
    c.gold = Bcd2(r, O_GOLD);
    c.gems = Bcd1(r[O_GEMS]);
    c.keys = Bcd1(r[O_KEYS]);
    c.powders = Bcd1(r[O_POWDERS]);
    c.torches = Bcd1(r[O_TORCHES]);

    uint8_t w = r[O_WEAPON_READY], a = r[O_ARMOUR_WORN];
    c.weapon = w < 16 ? std::wstring(WEAPONS[w]) : Unknown(w);
    c.armour = a < 8 ? std::wstring(ARMOUR[a]) : Unknown(a);

    for (size_t i = 1; i < 16; ++i) {
        int n = Bcd1(r[O_WEAPON_INV + i - 1]);
        if (n > 0) c.carried.emplace_back(WEAPONS[i], n);
    }
    for (size_t i = 1; i < 8; ++i) {
        int n = Bcd1(r[O_ARMOUR_INV + i - 1]);
        if (n > 0) c.carried.emplace_back(ARMOUR[i], n);
    }
    return c;
}

std::vector<std::pair<DWORD, std::wstring>> FindDosBox() {
    std::vector<std::pair<DWORD, std::wstring>> found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        std::wstring lower = entry.szExeFile;
        for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        if (lower.find(L"dosbox") != std::wstring::npos) found.emplace_back(entry.th32ProcessID, entry.szExeFile);
    }
    CloseHandle(snap);
    return found;
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
    for (int i = 0; i < 4; ++i) {
        p.slots[i] = raw[H_SLOTS + i];
        p.chars[i] = DecodeCharacter(raw + HEADER_SIZE + i * RECORD_SIZE);
    }
    return p;
}

DosBoxReader::~DosBoxReader() { Close(); }

void DosBoxReader::Close() {
    if (handle_) CloseHandle(handle_);
    handle_ = nullptr;
    pid = 0;
    canWrite = false;
    exe.clear();
    candidates.clear();
    index = 0;
}

uint64_t DosBoxReader::Address() const { return index < candidates.size() ? candidates[index] : 0; }

bool DosBoxReader::Attach() {
    auto procs = FindDosBox();
    if (procs.empty()) {
        Close();
        lastError = L"DOSBox is not running.";
        return false;
    }
    if (handle_)
        for (const auto& p : procs)
            if (p.first == pid) return true;
    Close();
    constexpr DWORD READ_ACCESS = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    constexpr DWORD WRITE_ACCESS = READ_ACCESS | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    for (const auto& p : procs) {
        // Ask for write access so the edit actions work; settle for reading.
        HANDLE h = OpenProcess(WRITE_ACCESS, FALSE, p.first);
        const bool writable = h != nullptr;
        if (!h) h = OpenProcess(READ_ACCESS, FALSE, p.first);
        if (h) {
            handle_ = h;
            pid = p.first;
            exe = p.second;
            canWrite = writable;
            lastError.clear();
            return true;
        }
    }
    // Found DOSBox but could not open it -- almost always elevation.
    pid = procs[0].first;
    exe = procs[0].second;
    lastError = L"Cannot open " + exe + L" (pid " + std::to_wstring(pid) +
                L") — try running this tool as administrator.";
    return false;
}

bool DosBoxReader::Read(uint64_t address, uint8_t* buffer, size_t size) const {
    if (!handle_) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(address), buffer, size, &got) && got == size;
}

void DosBoxReader::Scan() {
    candidates.clear();
    index = 0;
    if (!handle_) return;

    constexpr DWORD READABLE = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr size_t CHUNK = 4 << 20;
    constexpr size_t OVERLAP = PARTY_SIZE + RECORD_SIZE * 4;

    std::map<uint64_t, uint64_t> found;  // party address -> size of the region holding it
    std::vector<uint8_t> buf(CHUNK);
    PartyBytes block;
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t addr = 0;

    while (VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi)) {
        uint64_t base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        uint64_t size = mbi.RegionSize;
        if (size == 0) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & READABLE) && !(mbi.Protect & PAGE_GUARD) &&
            size >= 0x10000 && size <= (1ull << 30)) {
            for (uint64_t pos = 0; pos < size; pos += CHUNK - OVERLAP) {
                size_t n = static_cast<size_t>(std::min<uint64_t>(CHUNK, size - pos));
                if (Read(base + pos, buf.data(), n)) {
                    for (size_t i = 0; i + 0x19 <= n; ++i) {
                        if (buf[i + O_INUSE] != 0xFF || !MatchesSignature(&buf[i])) continue;
                        uint64_t record = base + pos + i;
                        // The match may be any of the four records.
                        for (size_t k = 0; k < 4; ++k) {
                            uint64_t back = HEADER_SIZE + k * RECORD_SIZE;
                            if (record < back) break;
                            uint64_t party = record - back;
                            if (Read(party, block.data(), PARTY_SIZE) && LooksLikeParty(block.data())) {
                                found[party] = std::max(found[party], size);
                                break;
                            }
                        }
                    }
                }
                if (n < CHUNK) break;
            }
        }
        addr = base + size;
    }

    // Prefer the biggest region: DOSBox's emulated RAM is one large
    // allocation, while stray copies sit in small heap blocks.
    std::vector<std::pair<uint64_t, uint64_t>> ranked(found.begin(), found.end());
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& r : ranked) candidates.push_back(r.first);
}

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
        result.message = L"DOSBox could only be opened read-only — run U3Stats as administrator to change values.";
        return false;
    }
    return true;
}

bool DosBoxReader::WriteBcd2(int member, size_t offset, int value) {
    const uint8_t bytes[2] = {ToBcd(value % 100), ToBcd(value / 100 % 100)};
    const uint64_t at = candidates[index] + HEADER_SIZE + member * RECORD_SIZE + offset;
    SIZE_T written = 0;
    return WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(at), bytes, sizeof bytes, &written) &&
           written == sizeof bytes;
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
    for (int i = 0; i < count; ++i) {
        const int value = share + (i < extra ? 1 : 0);
        if (value != food[i] && !WriteBcd2(i, O_FOOD, value)) {
            result.message = L"Writing to DOSBox failed part-way — check food in game.";
            return result;
        }
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
    if (gold[target] != before[target] && !WriteBcd2(target, O_GOLD, gold[target])) {
        result.message = L"Writing to DOSBox failed — nothing was changed.";
        return result;
    }
    for (int i = 0; i < count; ++i) {
        if (i != target && gold[i] != before[i] && !WriteBcd2(i, O_GOLD, gold[i])) {
            result.message = L"Writing to DOSBox failed part-way — check gold in game.";
            return result;
        }
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

}  // namespace u3
