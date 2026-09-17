// mapdata.h -- Ultima III's maps: where each one is, reading them from the
// game's files, turning them into pixels and cell symbols, and remembering
// which dungeon cells have been explored.
//
// World, town and castle maps are 64 x 64 bytes, each a tile number times four,
// drawn with the 16 x 16 tiles in SHAPES.VGA and the palette in U3VGA.PAL.
// Dungeon files hold eight 16 x 16 levels of cell codes, which EXODUS.BIN's
// step handlers give their meaning. The location tables (files and entrance
// coordinates) are EXODUS.BIN's own, at 16BB and 16E1.
//
// Colours are 0xRRGGBB.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "reader.h"

namespace u3 {

constexpr int WORLD_SIZE = 64, DUNGEON_SIZE = 16, DUNGEON_LEVELS = 8, TILE = 16, TILE_COUNT = 80;
constexpr int WORLD_PIXELS = WORLD_SIZE * TILE;

struct Place {
    const wchar_t* name;
    const wchar_t* file;
    int entryX, entryY;  // where its entrance is on Sosaria; -1 for the two worlds
};

extern const Place WORLD_PLACES[];
extern const int WORLD_PLACE_COUNT;
constexpr int SAVED_WORLDS = 2;  // the first two world places: maps the game saves changes to

extern const Place DUNGEON_PLACES[];
extern const int DUNGEON_PLACE_COUNT;

// The world place (index into WORLD_PLACES) or dungeon the party is in, or -1.
int WorldPlaceOf(const Location& where);
int DungeonOf(const Location& where);

// A game file of at least minSize bytes, or empty. With preferSaved, the
// game's saved copy under cloud_saves wins when there is one.
std::vector<uint8_t> LoadGameFile(const std::wstring& folder, const wchar_t* name, bool preferSaved, size_t minSize);

struct Tiles {
    std::vector<uint8_t> shapes, palette;  // both empty if they couldn't be read

    void Load(const std::wstring& folder);
    bool Loaded() const { return !shapes.empty() && !palette.empty(); }
};

// A whole world map, WORLD_PIXELS square, row by row. Without tiles, each
// tile is a plain grey.
std::vector<uint32_t> RenderWorld(const std::vector<uint8_t>& map, const Tiles& tiles);

// How to draw one dungeon cell.
struct CellLook {
    uint32_t fill;
    const wchar_t* mark;  // nullptr for none
    uint32_t ink;         // the mark's colour
};

CellLook DungeonCell(uint8_t value, bool explored);

constexpr uint32_t DUNGEON_GRID = 0x464646;
constexpr uint32_t PARTY_ARROW = 0xDC0000, PARTY_ARROW_EDGE = 0xFFFFFF;  // the dungeon cursor
constexpr uint32_t PARTY_BOX = 0xFFE600, PARTY_BOX_EDGE = 0x000000;      // the world cursor

extern const wchar_t* const DUNGEON_LEGEND;

// One bit per cell of a dungeon level.
using ExploredCells = std::array<uint8_t, (DUNGEON_SIZE * DUNGEON_SIZE + 7) / 8>;

bool IsExplored(const ExploredCells& cells, int x, int y);
// Marks the cell and its eight neighbours (wrapping at the edges); returns
// whether any were new.
bool ExploreAround(ExploredCells& cells, int x, int y);
std::wstring ExploredToHex(const ExploredCells& cells);
ExploredCells ExploredFromHex(const std::wstring& hex);

// A name for one dungeon level's explored cells, e.g. "FIRE.ULT.3".
std::wstring ExploredKey(int dungeon, int level);

}  // namespace u3
