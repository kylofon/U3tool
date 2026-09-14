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

// Numeric fields are -1 when the byte isn't valid BCD.
struct Character {
    bool present = false;
    std::wstring name, status, race, klass, sex, weapon, armour;
    char statusCode = '?';
    int strength = -1, dexterity = -1, intelligence = -1, wisdom = -1;
    int mp = -1, hp = -1, maxHp = -1, exp = -1, food = -1, gold = -1;
    int gems = -1, keys = -1, powders = -1, torches = -1;
    std::vector<std::pair<std::wstring, int>> carried;  // weapons, then armour
};

struct Party {
    int count = 0;
    uint8_t slots[4] = {};
    Character chars[4];
};

struct ActionResult {
    bool ok = false;
    std::wstring message;
};

bool LooksLikeParty(const uint8_t* raw);
Party DecodeParty(const uint8_t* raw);

class DosBoxReader {
public:
    ~DosBoxReader();

    bool Attach();
    void Scan();
    void NextCandidate();
    bool Poll(PartyBytes& out);  // re-reads, rescanning if the cached address went bad
    uint64_t Address() const;

    // Party edits. Each re-reads the live block first and writes back only
    // the two-byte fields it changes.
    ActionResult DistributeFood();
    ActionResult PoolGold(int member);

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
    bool WriteBcd2(int member, size_t offset, int value);

    HANDLE handle_ = nullptr;
};

}  // namespace u3
