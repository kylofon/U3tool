// mapdata.cpp -- Ultima III's map tables, map files and dungeon cells.
#include "mapdata.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <system_error>

namespace u3 {
namespace {

namespace fs = std::filesystem;

constexpr int MAP_SOSARIA = 0x00, MAP_DUNGEON = 0x01, MAP_SHRINE = 0x04, MAP_AMBROSIA = 0xFF;

std::vector<uint8_t> ReadWholeFile(const fs::path& path) {
    std::vector<uint8_t> data;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size >= (1 << 20)) return data;
    std::ifstream in(path, std::ios::binary);
    data.resize(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) data.clear();
    return data;
}

}  // namespace

const Place WORLD_PLACES[] = {
    {L"Sosaria", L"SOSARIA.ULT", -1, -1},
    {L"Ambrosia", L"AMBROSIA.ULT", -1, -1},
    {L"Castle of Lord British", L"BRITISH.ULT", 0x2D, 0x12},
    {L"Britain", L"LCB.ULT", 0x2E, 0x13},
    {L"Castle Death", L"EXODUS.ULT", 0x0A, 0x35},
    {L"Moon", L"MOON.ULT", 0x06, 0x0D},
    {L"Yew", L"YEW.ULT", 0x22, 0x10},
    {L"Montor East", L"MONTOR_E.ULT", 0x31, 0x3A},
    {L"Montor West", L"MONTOR_W.ULT", 0x2F, 0x3A},
    {L"Grey", L"GREY.ULT", 0x07, 0x2C},
    {L"Dawn", L"DAWN.ULT", 0x25, 0x35},
    {L"Devil Guard", L"DEVIL.ULT", 0x12, 0x1F},
    {L"Fawn", L"FAWN.ULT", 0x1E, 0x02},
    {L"Death Gulch", L"DEATH.ULT", 0x38, 0x1F},
};
const int WORLD_PLACE_COUNT = sizeof WORLD_PLACES / sizeof WORLD_PLACES[0];

const Place DUNGEON_PLACES[] = {
    {L"Dungeon of Doom", L"M.ULT", 0x13, 0x39},
    {L"Dungeon of Fire", L"FIRE.ULT", 0x31, 0x22},
    {L"Dungeon of Time", L"TIME.ULT", 0x3A, 0x1E},
    {L"Dungeon of the Snake", L"P.ULT", 0x3A, 0x2C},
    {L"Perinian Depths", L"PERINIAN.ULT", 0x38, 0x06},
    {L"Mines of Morinia", L"MINE.ULT", 0x09, 0x1C},
    {L"Dardin's Pit", L"DARDIN.ULT", 0x2E, 0x07},
};
const int DUNGEON_PLACE_COUNT = sizeof DUNGEON_PLACES / sizeof DUNGEON_PLACES[0];

const wchar_t* const DUNGEON_LEGEND =
    L"▲ ladder up    ▼ ladder down    ⇕ both    $ chest    F fountain    ! trap    W strange wind    "
    L"M mark (red-hot rod)    G gremlins    ? misty writing    T Time Lord    S secret door.    "
    L"Dark squares are unexplored; the red arrow is the party, pointing the way it faces.";

int WorldPlaceOf(const Location& where) {
    if (!where.live) return -1;
    if (where.map == MAP_SOSARIA) return 0;
    if (where.map == MAP_AMBROSIA) return 1;
    if (where.map >= 0x02 && where.map < 0x10 && where.map != MAP_SHRINE)
        for (int i = SAVED_WORLDS; i < WORLD_PLACE_COUNT; ++i)
            if (WORLD_PLACES[i].entryX == where.entryX && WORLD_PLACES[i].entryY == where.entryY) return i;
    return -1;
}

int DungeonOf(const Location& where) {
    if (!where.live || where.map != MAP_DUNGEON) return -1;
    for (int i = 0; i < DUNGEON_PLACE_COUNT; ++i)
        if (DUNGEON_PLACES[i].entryX == where.entryX && DUNGEON_PLACES[i].entryY == where.entryY) return i;
    return -1;
}

// The game saves its changes to Sosaria and Ambrosia under cloud_saves.
std::vector<uint8_t> LoadGameFile(const std::wstring& folder, const wchar_t* name, bool preferSaved, size_t minSize) {
    const fs::path base(folder);
    if (preferSaved) {
        std::vector<uint8_t> saved = ReadWholeFile(base / L"cloud_saves" / name);
        if (saved.size() >= minSize) return saved;
    }
    std::vector<uint8_t> data = ReadWholeFile(base / name);
    if (data.size() < minSize) data.clear();
    return data;
}

void Tiles::Load(const std::wstring& folder) {
    shapes = LoadGameFile(folder, L"SHAPES.VGA", false, TILE_COUNT * TILE * TILE);
    palette = LoadGameFile(folder, L"U3VGA.PAL", false, 768);
}

std::vector<uint32_t> RenderWorld(const std::vector<uint8_t>& map, const Tiles& tiles) {
    std::vector<uint32_t> out(WORLD_PIXELS * WORLD_PIXELS);
    if (map.size() < size_t(WORLD_SIZE * WORLD_SIZE)) return out;
    const bool haveTiles = tiles.Loaded();
    for (int my = 0; my < WORLD_SIZE; ++my) {
        for (int mx = 0; mx < WORLD_SIZE; ++mx) {
            const int tile = std::min(map[my * WORLD_SIZE + mx] >> 2, TILE_COUNT - 1);
            for (int y = 0; y < TILE; ++y) {
                for (int x = 0; x < TILE; ++x) {
                    uint32_t colour;
                    if (haveTiles) {
                        const uint8_t i = tiles.shapes[tile * TILE * TILE + y * TILE + x];  // VGA palette is 0-63
                        colour = uint32_t(tiles.palette[i * 3] * 4) << 16 |
                                 uint32_t(tiles.palette[i * 3 + 1] * 4) << 8 | uint32_t(tiles.palette[i * 3 + 2] * 4);
                    } else {
                        colour = 0x303030 + uint32_t(tile) * 0x020202;
                    }
                    out[(my * TILE + y) * WORLD_PIXELS + mx * TILE + x] = colour;
                }
            }
        }
    }
    return out;
}

CellLook DungeonCell(uint8_t value, bool explored) {
    constexpr uint32_t WHITE = 0xFFFFFF, DOOR = 0x966432, FLOOR = 0xE8E2D2;
    if (!explored) return {0x202020, nullptr, WHITE};
    switch (value) {
        case 0x80: return {0x646464, nullptr, WHITE};  // wall
        case 0xC0: return {DOOR, nullptr, WHITE};
        case 0xA0: return {DOOR, L"S", WHITE};  // secret door
        case 0x00: return {FLOOR, nullptr, WHITE};
        case 0x01: return {FLOOR, L"T", 0x7828A0};
        case 0x02: return {FLOOR, L"F", 0x006EC8};
        case 0x03: return {FLOOR, L"W", 0x5A5A5A};
        case 0x04: return {FLOOR, L"!", 0xC80000};
        case 0x05: return {FLOOR, L"M", 0xC84600};
        case 0x06: return {FLOOR, L"G", 0x008200};
        case 0x08: return {FLOOR, L"?", 0x785028};
        case 0x10: return {FLOOR, L"▲", 0x005AC8};
        case 0x20: return {FLOOR, L"▼", 0x005AC8};
        case 0x30: return {FLOOR, L"⇕", 0x005AC8};
        case 0x40: return {FLOOR, L"$", 0xBE8C00};
        default: return {FLOOR, L"·", 0x787878};
    }
}

bool IsExplored(const ExploredCells& cells, int x, int y) {
    const int i = y * DUNGEON_SIZE + x;
    return (cells[i >> 3] >> (i & 7) & 1) != 0;
}

// The cells the dungeon view draws, relative to the party facing north
// (negative y is ahead), numbered as in the game's dungeon drawing code
// (its tables at 0107 and 0127). Some cells appear twice, once for each face
// that can be seen:
//
//   15 16/1C 17/1D 18 19/1E 1A/1F 1B      three ahead
//   0F 0A/10 0B/11 0C 0D/12 0E/13 14      two ahead
//         06 03/07 04 05/08 09            one ahead
//               01 00 02                  the party's row
namespace {

struct ViewCell {
    int x, y;
};
constexpr ViewCell VIEW_CELLS[32] = {
    {0, 0},   {-1, 0},  {1, 0},   {-1, -1}, {0, -1},  {1, -1},  {-2, -1}, {-1, -1},
    {1, -1},  {2, -1},  {-2, -2}, {-1, -2}, {0, -2},  {1, -2},  {2, -2},  {-3, -2},
    {-2, -2}, {-1, -2}, {1, -2},  {2, -2},  {3, -2},  {-3, -3}, {-2, -3}, {-1, -3},
    {0, -3},  {1, -3},  {2, -3},  {3, -3},  {-2, -3}, {-1, -3}, {1, -3},  {2, -3},
};

}  // namespace

bool ExploreView(ExploredCells& cells, const uint8_t* level, int x, int y, int facing, bool lit) {
    bool changed = false;
    constexpr int OPEN = 0, BLOCKED = -1, DOORWAY = 1;

    // Marks one view cell as seen and says whether it blocks the view, as
    // draw_dungeon_block does: walls, secret doors and doors do, except a door
    // the party stands in.
    auto see = [&](int block) {
        int dx = VIEW_CELLS[block].x, dy = VIEW_CELLS[block].y;
        for (int turn = 0; turn < facing; ++turn) {  // a quarter turn clockwise each
            const int nx = -dy;
            dy = dx;
            dx = nx;
        }
        const int cx = (x + dx) & (DUNGEON_SIZE - 1), cy = (y + dy) & (DUNGEON_SIZE - 1);
        const int i = cy * DUNGEON_SIZE + cx;
        if (!(cells[i >> 3] & (1 << (i & 7)))) {
            cells[i >> 3] |= static_cast<uint8_t>(1 << (i & 7));
            changed = true;
        }
        const uint8_t value = level[i];
        if (!(value & 0x80)) return OPEN;
        return value >= 0xC0 && block == 0 ? DOORWAY : BLOCKED;
    };
    // Cells along one line of sight, up to and including the first that blocks it.
    auto line = [&](std::initializer_list<int> blocks) {
        for (int block : blocks)
            if (see(block) != OPEN) break;
    };

    // The party's own cell is known even in the dark; the rest needs a torch,
    // and follows the order dungeon_main draws in.
    if (!lit) {
        see(0);
        return changed;
    }
    const int here = see(0);
    if (here == BLOCKED) return changed;  // inside a wall
    if (here == OPEN) {                   // from a doorway only the way ahead is seen
        line({0x01, 0x03, 0x06, 0x0A, 0x0F, 0x15});
        line({0x02, 0x05, 0x09, 0x0E, 0x14, 0x1B});
    }
    if (see(0x04) != OPEN) return changed;
    line({0x07, 0x0B, 0x10, 0x16});
    line({0x08, 0x0D, 0x13, 0x1A});
    if (see(0x0C) != OPEN) return changed;
    line({0x11, 0x17, 0x1C});
    line({0x12, 0x19, 0x1F});
    if (see(0x18) != OPEN) return changed;
    see(0x1D);
    see(0x1E);
    return changed;
}

std::wstring ExploredToHex(const ExploredCells& cells) {
    static const wchar_t HEX[] = L"0123456789abcdef";
    std::wstring hex;
    for (uint8_t b : cells) {
        hex += HEX[b >> 4];
        hex += HEX[b & 15];
    }
    return hex;
}

ExploredCells ExploredFromHex(const std::wstring& hex) {
    ExploredCells cells{};
    for (size_t i = 0; i + 1 < hex.size() && i / 2 < cells.size(); i += 2)
        cells[i / 2] = static_cast<uint8_t>(std::wcstoul(hex.substr(i, 2).c_str(), nullptr, 16));
    return cells;
}

std::wstring ExploredKey(int dungeon, int level) {
    return std::wstring(DUNGEON_PLACES[dungeon].file) + L"." + std::to_wstring(level + 1);
}

}  // namespace u3
