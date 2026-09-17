// emulator.h -- the ways into the emulated PC's memory: DOSBox Staging's HTTP
// API everywhere, and on Windows also reading a DOSBox process's memory.
//
// Addresses are opaque to callers: emulated linear addresses for the HTTP API,
// host addresses inside the DOSBox process otherwise. The reader only ever
// works with offsets from an address it found by scanning, so either will do.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace u3 {

enum class WriteResult {
    Ok,
    Changed,  // memory no longer held the expected bytes, so nothing was written
    Failed,
};

// Receives successive chunks of memory. Chunks overlap, so a pattern
// straddling a chunk edge is still seen whole; regionSize is the size of the
// block of memory the chunk came from.
using ChunkVisitor = std::function<void(uint64_t at, const uint8_t* bytes, size_t size, uint64_t regionSize)>;

class Emulator {
public:
    virtual ~Emulator() = default;

    // Finds the emulator, or confirms it's still there. On success `name`,
    // `pid` and `session` describe it; on failure `error` says why.
    virtual bool Connect() = 0;
    virtual bool CanWrite() const = 0;

    virtual bool Read(uint64_t address, uint8_t* buffer, size_t size) = 0;

    // With `expected` (size bytes), writes only if memory still holds exactly
    // that. The HTTP API checks and writes between two emulated instructions;
    // reading a process directly can only check just before writing.
    virtual WriteResult Write(uint64_t address, const uint8_t* bytes, size_t size, const uint8_t* expected) = 0;

    // Every block of memory the game could be in.
    virtual void ForEachChunk(const ChunkVisitor& visit) = 0;

    std::wstring name;     // what to call it in the UI, e.g. "DOSBox Staging 0.83.0 (HTTP API)"
    uint32_t pid = 0;      // 0 if unknown
    uint64_t session = 0;  // changes whenever a different emulator (or a restarted one) is connected
    std::wstring error;
};

// A fresh value for Emulator::session.
uint64_t NextSession();

// DOSBox Staging 0.83+ with `webserver_enabled = on`.
std::unique_ptr<Emulator> ConnectStaging(const std::wstring& host, int port);

// Any process with "dosbox" in its name: reads and writes its memory directly.
// Only on Windows; nullptr elsewhere.
std::unique_ptr<Emulator> ConnectProcess();

// The folder holding the game's files, judged from the emulator process's
// executable, working folder and the config files on its command line; empty
// if none of them tells.
std::wstring FindGameFolder(uint32_t pid);

// The process listening on a local TCP port, or 0.
uint32_t ListeningProcess(int port);

}  // namespace u3
