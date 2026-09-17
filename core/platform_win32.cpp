// platform_win32.cpp -- Windows only: direct DOSBox process memory access,
// finding which process owns a port, and finding the game's folder from a
// process.
#ifdef _WIN32

// winsock2.h has to come before windows.h.
#include <winsock2.h>
#include <windows.h>

#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include "emulator.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <utility>
#include <vector>

namespace u3 {
namespace {

std::vector<std::pair<DWORD, std::wstring>> FindDosBoxProcesses() {
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

class ProcessMemory final : public Emulator {
public:
    ~ProcessMemory() override { Close(); }

    bool Connect() override {
        auto procs = FindDosBoxProcesses();
        if (procs.empty()) {
            Close();
            error = L"DOSBox is not running.";
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
                name = p.second;
                canWrite_ = writable;
                session = NextSession();
                error.clear();
                return true;
            }
        }
        // Found DOSBox but could not open it -- almost always elevation.
        error = L"Cannot open " + procs[0].second + L" (pid " + std::to_wstring(procs[0].first) +
                L") — try running this tool as administrator.";
        return false;
    }

    bool CanWrite() const override { return canWrite_; }

    bool Read(uint64_t address, uint8_t* buffer, size_t size) override {
        if (!handle_) return false;
        SIZE_T got = 0;
        return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(address), buffer, size, &got) && got == size;
    }

    WriteResult Write(uint64_t address, const uint8_t* bytes, size_t size, const uint8_t* expected) override {
        if (!canWrite_) return WriteResult::Failed;
        if (expected) {
            std::vector<uint8_t> now(size);
            if (!Read(address, now.data(), size)) return WriteResult::Failed;
            if (std::memcmp(now.data(), expected, size) != 0) return WriteResult::Changed;
        }
        SIZE_T written = 0;
        return WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(address), bytes, size, &written) &&
                       written == size
                   ? WriteResult::Ok
                   : WriteResult::Failed;
    }

    // Every committed, readable region big enough to matter.
    void ForEachChunk(const ChunkVisitor& visit) override {
        constexpr DWORD READABLE = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        constexpr size_t CHUNK = 4 << 20;
        constexpr size_t OVERLAP = 1024;  // more than a party block plus a record either side

        if (!handle_) return;
        std::vector<uint8_t> buf(CHUNK);
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
                    if (Read(base + pos, buf.data(), n)) visit(base + pos, buf.data(), n, size);
                    if (n < CHUNK) break;
                }
            }
            addr = base + size;
        }
    }

private:
    void Close() {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
        pid = 0;
        canWrite_ = false;
        name.clear();
    }

    HANDLE handle_ = nullptr;
    bool canWrite_ = false;
};

bool HoldsGame(const std::wstring& folder) {
    return !folder.empty() && GetFileAttributesW((folder + L"\\SOSARIA.ULT").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// `path`'s folder, or one of the next few up, if it holds the game.
std::wstring GameFolderAbove(std::wstring path, int levels) {
    for (int up = 0; up < levels; ++up) {
        const size_t cut = path.find_last_of(L"\\/");
        if (cut == std::wstring::npos) break;
        path.resize(cut);
        if (HoldsGame(path)) return path;
    }
    return L"";
}

std::wstring CommandLine(HANDLE process) {
    constexpr PROCESSINFOCLASS COMMAND_LINE_INFORMATION = static_cast<PROCESSINFOCLASS>(60);  // Windows 8.1+
    std::vector<uint8_t> buffer(4096);
    ULONG needed = 0;
    NTSTATUS status = NtQueryInformationProcess(process, COMMAND_LINE_INFORMATION, buffer.data(),
                                                static_cast<ULONG>(buffer.size()), &needed);
    if (status < 0 && needed > buffer.size()) {
        buffer.resize(needed);
        status = NtQueryInformationProcess(process, COMMAND_LINE_INFORMATION, buffer.data(),
                                           static_cast<ULONG>(buffer.size()), &needed);
    }
    if (status < 0) return L"";
    const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    return std::wstring(text->Buffer, text->Length / sizeof(wchar_t));
}

}  // namespace

std::unique_ptr<Emulator> ConnectProcess() { return std::make_unique<ProcessMemory>(); }

uint32_t ListeningProcess(int port) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
        ERROR_INSUFFICIENT_BUFFER)
        return 0;
    std::vector<uint8_t> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return 0;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
        if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port) return table->table[i].dwOwningPid;
    return 0;
}

std::wstring FindGameFolder(uint32_t pid) {
    if (!pid) return L"";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";

    std::wstring found;
    // GOG installs DOSBox in a folder inside the game's.
    wchar_t image[MAX_PATH];
    DWORD length = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, image, &length)) found = GameFolderAbove(std::wstring(image, length), 3);

    // A DOSBox installed elsewhere is usually handed the game's config files.
    if (found.empty()) {
        const std::wstring line = CommandLine(process);
        int argc = 0;
        if (LPWSTR* argv = line.empty() ? nullptr : CommandLineToArgvW(line.c_str(), &argc)) {
            // Only absolute paths: a relative one depends on DOSBox's working folder.
            auto absolute = [](const wchar_t* p) { return (p[0] && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\'); };
            for (int i = 1; i + 1 < argc && found.empty(); ++i)
                if (_wcsicmp(argv[i], L"-conf") == 0 && absolute(argv[i + 1]))
                    found = GameFolderAbove(argv[i + 1], 2);
            LocalFree(argv);
        }
    }
    CloseHandle(process);
    return found;
}

}  // namespace u3

#endif  // _WIN32
