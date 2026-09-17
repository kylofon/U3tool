// staging.cpp -- DOSBox Staging's HTTP API client, built on cpp-httplib so it
// works the same on Windows and Linux.
#include "third_party/cpp-httplib/httplib.h"

#include "emulator.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace u3 {
namespace {

std::wstring Widen(const std::string& s) {
    std::wstring out;
    for (unsigned char ch : s) out += static_cast<wchar_t>(ch);  // the API's JSON is ASCII
    return out;
}

std::string Narrow(const std::wstring& s) {
    std::string out;
    for (wchar_t ch : s) out += ch < 0x80 ? static_cast<char>(ch) : '?';  // host names and addresses are ASCII
    return out;
}

std::string Base64(const uint8_t* bytes, size_t size) {
    static const char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = bytes[i] << 16 | (i + 1 < size ? bytes[i + 1] << 8 : 0) | (i + 2 < size ? bytes[i + 2] : 0);
        out += TABLE[n >> 18 & 63];
        out += TABLE[n >> 12 & 63];
        out += i + 1 < size ? TABLE[n >> 6 & 63] : '=';
        out += i + 2 < size ? TABLE[n & 63] : '=';
    }
    return out;
}

// The string value of `key` in a flat JSON object; enough for /dosbox/info.
std::string JsonString(const std::string& json, const std::string& key) {
    size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    at = json.find(':', at);
    if (at == std::string::npos) return "";
    at = json.find('"', at);
    if (at == std::string::npos) return "";
    const size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string MemoryPath(uint64_t address) { return "/api/v1/memory/" + Hex(address); }

class StagingApi final : public Emulator {
public:
    StagingApi(const std::wstring& host, int port) : host_(host), port_(port), client_(Narrow(host), port) {
        // The API answers between two emulated instructions, so a long wait
        // means DOSBox is paused or gone.
        client_.set_connection_timeout(std::chrono::seconds(1));
        client_.set_read_timeout(std::chrono::seconds(3));
        client_.set_write_timeout(std::chrono::seconds(2));
        client_.set_keep_alive(true);
    }

    bool Connect() override {
        if (connected_) return true;
        // A refused connection is quick, but there's no need to knock every poll.
        const auto now = std::chrono::steady_clock::now();
        if (probed_ && now - lastProbe_ < std::chrono::milliseconds(900)) return false;
        probed_ = true;
        lastProbe_ = now;

        auto res = client_.Get("/api/v1/dosbox/info");
        if (!res || res->status != 200) {
            error = L"DOSBox Staging's HTTP API isn't answering on " + host_ + L":" + std::to_wstring(port_) + L".";
            return false;
        }
        const std::string version = JsonString(res->body, "version");
        name = L"DOSBox Staging" + (version.empty() ? L"" : L" " + Widen(version)) + L" (HTTP API)";
        pid = ListeningProcess(port_);
        session = NextSession();
        error.clear();
        connected_ = true;
        return true;
    }

    bool CanWrite() const override { return true; }

    bool Read(uint64_t address, uint8_t* buffer, size_t size) override {
        auto res = client_.Get(MemoryPath(address) + "/" + Hex(size));
        if (!res) return Lost();
        if (res->status != 200 || res->body.size() != size) return false;
        std::memcpy(buffer, res->body.data(), size);
        return true;
    }

    WriteResult Write(uint64_t address, const uint8_t* bytes, size_t size, const uint8_t* expected) override {
        httplib::Headers headers;
        if (expected) headers.emplace("If-Match", "\"" + Base64(expected, size) + "\"");
        auto res = client_.Put(MemoryPath(address), headers, reinterpret_cast<const char*>(bytes), size,
                               "application/octet-stream");
        if (!res) {
            Lost();
            return WriteResult::Failed;
        }
        switch (res->status) {
            case 200: return WriteResult::Ok;
            case 412: return WriteResult::Changed;
            default: return WriteResult::Failed;
        }
    }

    void ForEachChunk(const ChunkVisitor& visit) override {
        // Ultima III lives in conventional memory, so the first megabyte is
        // all there is to search, and it comes in one request.
        constexpr size_t CONVENTIONAL = 0x100000;
        std::vector<uint8_t> memory(CONVENTIONAL);
        if (Read(0, memory.data(), memory.size())) visit(0, memory.data(), memory.size(), memory.size());
    }

private:
    // The request didn't get through, so Connect() has to find the API again.
    bool Lost() {
        if (connected_) error = L"Lost the connection to DOSBox Staging's HTTP API.";
        connected_ = false;
        return false;
    }

    std::wstring host_;
    int port_;
    httplib::Client client_;
    bool connected_ = false, probed_ = false;
    std::chrono::steady_clock::time_point lastProbe_;
};

}  // namespace

uint64_t NextSession() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

std::unique_ptr<Emulator> ConnectStaging(const std::wstring& host, int port) {
    return std::make_unique<StagingApi>(host, port);
}

}  // namespace u3
