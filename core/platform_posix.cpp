// platform_posix.cpp -- Linux: finding which process owns a port, and the
// game's folder from a process, through /proc. There's no direct process
// memory access here; the HTTP API is the way in.
#ifndef _WIN32

#include <dirent.h>
#include <unistd.h>

#include "emulator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace u3 {
namespace {

namespace fs = std::filesystem;

std::string ReadLink(const std::string& path) {
    std::error_code ec;
    const fs::path target = fs::read_symlink(path, ec);
    return ec ? std::string() : target.string();
}

bool HoldsGame(const fs::path& folder) {
    std::error_code ec;
    return !folder.empty() && fs::exists(folder / "SOSARIA.ULT", ec);
}

// `path` itself or one of the next few folders up, if it holds the game.
fs::path GameFolderFrom(fs::path path, int levels) {
    for (int up = 0; up <= levels && !path.empty(); ++up) {
        if (HoldsGame(path)) return path;
        if (path == path.parent_path()) break;
        path = path.parent_path();
    }
    return {};
}

// Socket inodes listening on `port` over IPv4 or IPv6.
std::set<std::string> ListeningInodes(int port) {
    std::set<std::string> inodes;
    for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        std::ifstream in(table);
        std::string line;
        std::getline(in, line);  // column titles
        while (std::getline(in, line)) {
            // local_address rem_address state ... inode, with the address as HEX:PORT in hex
            std::istringstream fields(line);
            std::string skip, local, remote, state, inode;
            fields >> skip >> local >> remote >> state;
            for (int i = 0; i < 5; ++i) fields >> skip;  // tx/rx queues, timer, retransmits, uid, timeout
            fields >> inode;
            const size_t colon = local.rfind(':');
            if (state != "0A" || colon == std::string::npos) continue;  // 0A is LISTEN
            if (std::strtol(local.c_str() + colon + 1, nullptr, 16) == port) inodes.insert(inode);
        }
    }
    return inodes;
}

}  // namespace

std::unique_ptr<Emulator> ConnectProcess() { return nullptr; }

uint32_t ListeningProcess(int port) {
    const std::set<std::string> inodes = ListeningInodes(port);
    if (inodes.empty()) return 0;
    DIR* proc = opendir("/proc");
    if (!proc) return 0;
    uint32_t found = 0;
    while (dirent* entry = readdir(proc)) {
        const char* name = entry->d_name;
        if (*name < '1' || *name > '9') continue;
        const std::string fds = std::string("/proc/") + name + "/fd";
        DIR* dir = opendir(fds.c_str());  // only our own processes can be looked into
        if (!dir) continue;
        while (dirent* fd = readdir(dir)) {
            const std::string target = ReadLink(fds + "/" + fd->d_name);
            if (target.rfind("socket:[", 0) == 0 && inodes.count(target.substr(8, target.size() - 9))) {
                found = static_cast<uint32_t>(std::strtoul(name, nullptr, 10));
                break;
            }
        }
        closedir(dir);
        if (found) break;
    }
    closedir(proc);
    return found;
}

std::wstring FindGameFolder(uint32_t pid) {
    if (!pid) return L"";
    const std::string proc = "/proc/" + std::to_string(pid);
    const fs::path cwd = ReadLink(proc + "/cwd");

    // GOG's layout runs DOSBox from a folder inside the game's.
    fs::path found = GameFolderFrom(fs::path(ReadLink(proc + "/exe")).parent_path(), 2);
    if (found.empty() && !cwd.empty()) found = GameFolderFrom(cwd, 1);

    // Otherwise the game's config files are usually on the command line.
    if (found.empty()) {
        std::ifstream in(proc + "/cmdline", std::ios::binary);
        std::vector<std::string> args;
        for (std::string arg; std::getline(in, arg, '\0');) args.push_back(arg);
        for (size_t i = 1; i + 1 < args.size() && found.empty(); ++i) {
            if (args[i] != "-conf") continue;
            fs::path conf = args[i + 1];
            if (conf.is_relative()) {
                if (cwd.empty()) continue;
                conf = cwd / conf;
            }
            found = GameFolderFrom(conf.lexically_normal().parent_path(), 1);
        }
    }
    return found.wstring();
}

}  // namespace u3

#endif  // !_WIN32
