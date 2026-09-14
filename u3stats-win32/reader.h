// reader.h -- find and decode the live Ultima III party block inside DOSBox.
//
// Ultima III keeps the party in RAM in exactly the layout it writes to
// PARTY.ULT: an 18-byte header followed by four 64-byte character records.
// Numbers are packed BCD, little endian (two bytes == four decimal digits).
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace u3 {

constexpr size_t RECORD_SIZE = 64;
constexpr size_t HEADER_SIZE = 18;
constexpr size_t PARTY_SIZE = HEADER_SIZE + 4 * RECORD_SIZE;  // 274

using PartyBytes = std::array<uint8_t, PARTY_SIZE>;

struct CarriedItem {
    std::wstring name;
    int count = 0;
    bool armour = false;  // otherwise a weapon
    int type = 0;         // weapons 1..15, armour 1..7
    bool equipped = false;  // the readied weapon or worn armour
};

// Numeric fields are -1 when the byte isn't valid BCD.
struct Character {
    bool present = false;
    std::wstring name, status, race, klass, sex, weapon, armour;
    char statusCode = '?';
    char classCode = '?';
    int strength = -1, dexterity = -1, intelligence = -1, wisdom = -1;
    int mp = -1, hp = -1, maxHp = -1, exp = -1, food = -1, gold = -1;
    int gems = -1, keys = -1, powders = -1, torches = -1;
    std::vector<CarriedItem> carried;  // weapons, then armour; the equipped one on a line of its own
};

struct Party {
    int count = 0;
    int map = 0;  // where the party is: 0x00 overworld, 0x01 dungeon, 0x80 combat, others for towns and castles
    uint8_t slots[4] = {};
    Character chars[4];
};

struct ActionResult {
    bool ok = false;
    std::wstring message;
};

// Ultima III is turn based, except that it passes a turn by itself when no key
// arrives within a few seconds. It times that against the DOS clock's seconds,
// so the wait is a whole number of seconds, 1..59.
constexpr int NORMAL_PASS_SECONDS = 5;
constexpr int MAX_PASS_SECONDS = 59;

struct GameSpeed {
    int passSeconds = NORMAL_PASS_SECONDS;
    bool paused = false;  // never pass a turn on its own

    bool operator==(const GameSpeed& o) const { return passSeconds == o.passSeconds && paused == o.paused; }
    bool operator!=(const GameSpeed& o) const { return !(*this == o); }
};

struct SpeedState {
    size_t sites = 0;      // idle-wait loops found in memory; 0 until the game proper is running
    GameSpeed actual;      // read back from the game after any change
    bool applied = false;  // every loop now matches the requested speed
    std::wstring error;
};

// Hand `count` of one item type from one party member to another.
struct ItemMove {
    int from = 0, to = 0;  // positions in the party
    bool armour = false;
    int type = 0;
    int count = 0;
};

bool LooksLikeParty(const uint8_t* raw);
Party DecodeParty(const uint8_t* raw);

// How many of the item `move.from` can hand to `move.to`: never the last of
// an equipped type, and no more than `to` has room for. 0 if none, with the
// reason in *why.
int MovableItems(const uint8_t* raw, const ItemMove& move, std::wstring* why);

// Ready a weapon or wear armour; type 0 puts the weapon away / takes armour off.
struct Equip {
    int member = 0;
    bool armour = false;
    int type = 0;
};

// Why the game's own Ready or Wear command would refuse this (dead, none
// owned, not allowed for the class), or an empty string if it wouldn't.
std::wstring EquipProblem(const uint8_t* raw, const Equip& equip);

// Ultima III's shops unequip a character's weapon (or armour) whenever they
// sell one, whatever was sold. Comparing two polls of the party, returns the
// equipment dropped that way that the character still owns.
std::vector<Equip> EquipmentLostToSale(const uint8_t* before, const uint8_t* after);

class DosBoxReader {
public:
    ~DosBoxReader();

    bool Attach();
    void Scan();
    void NextCandidate();
    bool Poll(PartyBytes& out);  // re-reads, rescanning if the cached address went bad
    uint64_t Address() const;
    int CombatTurn() const;  // in combat, the party member whose turn it is (0-3), or -1

    // Party edits. Each re-reads the live block first and writes back only
    // the two-byte fields it changes.
    ActionResult DistributeFood();
    ActionResult PoolGold(int member);
    ActionResult Revive(int member);      // Dead or Ashes back to Good, with full hit points
    ActionResult FullHealth(int member);  // hit points up to their maximum
    ActionResult Cure(int member);        // Poisoned back to Good
    ActionResult MoveItems(const ItemMove& move);
    ActionResult SetEquipped(const Equip& equip);

    // Patches the game's idle-wait loops to match `want`, re-finding them if the
    // game reloaded. Scanning for them is throttled, and skipped when !allowScan.
    SpeedState SyncSpeed(const GameSpeed& want, bool allowScan = true);

    DWORD pid = 0;
    std::wstring exe;
    std::wstring lastError;
    std::vector<uint64_t> candidates;
    size_t index = 0;
    bool canWrite = false;

private:
    void Close();
    bool Read(uint64_t address, uint8_t* buffer, size_t size) const;
    bool ReadLive(PartyBytes& out) const;
    bool BeginAction(PartyBytes& raw, ActionResult& result) const;
    bool Write(uint64_t address, const uint8_t* bytes, size_t size);
    bool WriteBcd2(int member, size_t offset, int value);
    void ScanIdleLoops();

    struct IdleLoop {
        uint64_t address;  // start of the loop's setup code
        size_t jumpAt;     // offset of its jnz/jmp opcode
    };

    HANDLE handle_ = nullptr;
    std::vector<IdleLoop> idleLoops_;
    ULONGLONG lastIdleScan_ = 0;
};

}  // namespace u3
