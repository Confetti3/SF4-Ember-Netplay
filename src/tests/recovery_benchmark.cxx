#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>
#include <psapi.h>
#include <bcrypt.h>

#include <ggponet.h>
#include <nlohmann/json.hpp>

#include "../platform/HelperClient.hxx"
#include "../platform/HelperProcess.hxx"
#include "../common/sf4e__PacingController.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace sf4e;

namespace {

struct Options {
    std::wstring helper;
    std::uint32_t seed = 0x5f4e2026u;
    int frames = 240;
    int dropEvery = 17;
    int delayMs = 2;
    int timeoutMs = 30000;
    bool skipNative = false;
    bool skipGgpo = false;
    bool relayOnly = false;
    // --rift: two independently clocked peers; see RunRift.
    bool rift = false;
    bool continuous = true; // --coarse selects the GGPO timesync event
    int jitterMs = 0, burstMs = 0, burstEveryMs = 0, inputDelay = 2;
    double fastHz = 60.5;
    std::wstring output;
};

struct ResourceSample {
    bool valid = false;
    std::uint64_t cpu100ns = 0;
    std::uint64_t workingSet = 0;
    std::uint64_t peakWorkingSet = 0;
};

ResourceSample SampleProcess(DWORD pid) {
    ResourceSample sample;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return sample;
    FILETIME creation{}, exit{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user) &&
        GetProcessMemoryInfo(process, &memory, sizeof(memory))) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        sample.valid = true;
        sample.cpu100ns = k.QuadPart + u.QuadPart;
        sample.workingSet = memory.WorkingSetSize;
        sample.peakWorkingSet = memory.PeakWorkingSetSize;
    }
    CloseHandle(process);
    return sample;
}

struct ResourcePair {
    ResourceSample host;
    ResourceSample guest;
};

ResourcePair SampleHelpers(const platform::HelperProcess& host, const platform::HelperProcess& guest) {
    return {SampleProcess(host.Bootstrap().helperPid), SampleProcess(guest.Bootstrap().helperPid)};
}

json ResourceJson(const ResourcePair& before, const ResourcePair& after, std::uint64_t sampledPeak) {
    const bool valid = before.host.valid && before.guest.valid && after.host.valid && after.guest.valid;
    if (!valid) return nullptr;
    const auto sum = [](const ResourceSample& a, const ResourceSample& b) {
        return a.cpu100ns + b.cpu100ns;
    };
    const auto working = [](const ResourceSample& a, const ResourceSample& b) {
        return a.workingSet + b.workingSet;
    };
    const auto peak = [](const ResourceSample& a, const ResourceSample& b) {
        return a.peakWorkingSet + b.peakWorkingSet;
    };
    const auto beforeWorking = working(before.host, before.guest);
    const auto afterWorking = working(after.host, after.guest);
    const auto beforeCpu = sum(before.host, before.guest);
    const auto afterCpu = sum(after.host, after.guest);
    return json{
        {"measured", afterCpu >= beforeCpu},
        // CPU and working-set deltas bracket forward stepping, confirmation,
        // and packet draining. Waiting for the next IPC statistic is excluded.
        {"helper_cpu_ms", afterCpu >= beforeCpu ? static_cast<double>(afterCpu - beforeCpu) / 10000.0 : 0.0},
        {"helper_cpu_delta_ms", afterCpu >= beforeCpu ? static_cast<double>(afterCpu - beforeCpu) / 10000.0 : 0.0},
        {"working_set_start_bytes", beforeWorking},
        {"working_set_end_bytes", afterWorking},
        {"working_set_growth_bytes", static_cast<std::int64_t>(afterWorking) - static_cast<std::int64_t>(beforeWorking)},
        {"sampled_peak_working_set_bytes", sampledPeak},
        {"process_lifetime_peak_working_set_bytes", peak(after.host, after.guest)},
    };
}

std::uint32_t NextRandom(std::uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

std::string NarrowPath(const std::wstring& path) {
    if (path.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    // The -1 source length asks Win32 to include the terminating NUL. Give
    // the conversion the full required capacity, then remove that NUL from
    // the returned path string.
    std::string result(static_cast<std::size_t>(length), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1,
        result.data(), length, nullptr, nullptr);
    if (written != length) return {};
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

std::string HexDigest(const std::vector<unsigned char>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

json LoadedGgpoArtifact() {
    HMODULE module = GetModuleHandleW(L"GGPO.dll");
    if (!module) return json{{"loaded", false}, {"path", nullptr}, {"sha256", nullptr}};
    std::vector<wchar_t> pathBuffer(32768);
    const auto length = GetModuleFileNameW(module, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (!length || length >= pathBuffer.size() - 1) return json{{"loaded", false}, {"path", nullptr}, {"sha256", nullptr}};
    const std::wstring path(pathBuffer.data(), length);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return json{{"loaded", true}, {"path", NarrowPath(path)}, {"sha256", nullptr}};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ULONG objectLength = 0, hashLength = 0, bytesRead = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    bool valid = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &bytesRead, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &bytesRead, 0) >= 0;
    if (valid) {
        object.resize(objectLength); digest.resize(hashLength);
        valid = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    while (valid) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { valid = false; break; }
        if (!read) break;
        valid = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (valid) valid = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return json{{"loaded", true}, {"path", NarrowPath(path)}, {"sha256", valid ? json(HexDigest(digest)) : json(nullptr)}};
}

std::array<std::uint8_t, 16> RoomBytes(const json& value) {
    return value.get<std::array<std::uint8_t, 16>>();
}

struct NativeGgpoContext {
    GGPOSession* session = nullptr;
    GGPOPlayerHandle local = GGPO_INVALID_HANDLE;
    GGPOPlayerHandle remote = GGPO_INVALID_HANDLE;
    std::int64_t currentFrame = 0;
    std::int64_t currentStateFrame = -1;
    std::uint64_t saves = 0, loads = 0, predictionStalls = 0, notSynchronized = 0;
    std::uint64_t inputDropped = 0, fatalErrors = 0, advances = 0, acceptedFrames = 0;
    std::uint64_t disconnectedEvents = 0, syncFailures = 0, runtimeErrors = 0;
    std::uint64_t callbackStalls = 0;
    std::vector<std::uint64_t> pingUs;
    int maxReplayDepth = 0;
    int initialChecksum = 0;
    int finalChecksum = 0;
    bool hasChecksum = false;
    std::uint64_t stateDigest = 1469598103934665603ULL;
    std::uint64_t initialStateDigest = 0;
    std::uint64_t finalStateDigest = 0;
    std::int64_t simulationFrame = 0;
    bool running = false;
};

NativeGgpoContext* activeNativeGgpo = nullptr;

struct NativeState {
    std::uint64_t digest = 0;
    std::int64_t frame = 0;
};

int StateChecksum(std::uint64_t digest) {
    return static_cast<int>((digest ^ (digest >> 32)) & 0x7fffffffU);
}

void MixNativeState(NativeGgpoContext& context, std::int64_t frame, const unsigned char* inputs, std::size_t length) {
    auto digest = context.stateDigest;
    digest ^= static_cast<std::uint64_t>(frame) + 0x9e3779b97f4a7c15ULL + (digest << 6) + (digest >> 2);
    for (std::size_t i = 0; i < length; ++i)
        digest = (digest ^ (static_cast<std::uint64_t>(inputs[i]) + i * 0x100000001b3ULL)) * 1099511628211ULL;
    context.stateDigest = digest;
    context.simulationFrame = frame + 1;
    context.finalStateDigest = digest;
    context.finalChecksum = StateChecksum(digest);
    if (!context.hasChecksum) {
        context.initialStateDigest = digest;
        context.initialChecksum = context.finalChecksum;
        context.hasChecksum = true;
    }
}

void RecordNativeGgpoError(NativeGgpoContext& context, GGPOErrorCode code) {
    if (code == GGPO_OK) return;
    if (code == GGPO_ERRORCODE_PREDICTION_THRESHOLD) ++context.predictionStalls;
    else if (code == GGPO_ERRORCODE_NOT_SYNCHRONIZED) ++context.notSynchronized;
    else if (code == GGPO_ERRORCODE_INPUT_DROPPED) ++context.inputDropped;
    else { ++context.fatalErrors; ++context.runtimeErrors; }
}

bool __cdecl NativeGgpoBegin(const char*) { return true; }
bool __cdecl NativeGgpoSave(unsigned char** buffer, int* length, int* checksum, int frame) {
    if (!activeNativeGgpo || !buffer || !length || !checksum) return false;
    if (activeNativeGgpo->simulationFrame != frame) {
        ++activeNativeGgpo->fatalErrors;
        return false;
    }
    NativeState state{activeNativeGgpo->stateDigest, static_cast<std::int64_t>(frame)};
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(state)));
    if (!*buffer) return false;
    std::memcpy(*buffer, &state, sizeof(state));
    *length = sizeof(state); *checksum = StateChecksum(state.digest);
    if (!activeNativeGgpo->hasChecksum) {
        activeNativeGgpo->initialStateDigest = state.digest;
        activeNativeGgpo->initialChecksum = *checksum;
        activeNativeGgpo->hasChecksum = true;
    }
    activeNativeGgpo->finalStateDigest = state.digest;
    activeNativeGgpo->finalChecksum = *checksum;
    activeNativeGgpo->currentStateFrame = state.frame;
    ++activeNativeGgpo->saves;
    return true;
}
bool __cdecl NativeGgpoLoad(unsigned char* buffer, int length) {
    if (!activeNativeGgpo || !buffer || length != static_cast<int>(sizeof(NativeState))) return false;
    NativeState saved{}; std::memcpy(&saved, buffer, sizeof(saved));
    ++activeNativeGgpo->loads;
    activeNativeGgpo->maxReplayDepth = std::max(activeNativeGgpo->maxReplayDepth,
        static_cast<int>(std::max<std::int64_t>(0, activeNativeGgpo->simulationFrame - saved.frame)));
    activeNativeGgpo->stateDigest = saved.digest;
    activeNativeGgpo->simulationFrame = saved.frame;
    activeNativeGgpo->currentStateFrame = saved.frame;
    activeNativeGgpo->finalStateDigest = saved.digest;
    activeNativeGgpo->finalChecksum = StateChecksum(saved.digest);
    return true;
}
bool __cdecl NativeGgpoLog(char*, unsigned char*, int) { return true; }
void __cdecl NativeGgpoFree(void* buffer) { std::free(buffer); }
bool __cdecl NativeGgpoAdvance(int) {
    if (!activeNativeGgpo || !activeNativeGgpo->session) return false;
    unsigned char inputs[2]{}; int disconnected = 0;
    const auto sync = ggpo_synchronize_input(activeNativeGgpo->session, inputs, sizeof(inputs), &disconnected);
    RecordNativeGgpoError(*activeNativeGgpo, sync);
    if (disconnected) { ++activeNativeGgpo->disconnectedEvents; ++activeNativeGgpo->fatalErrors; return false; }
    if (sync != GGPO_OK) { ++activeNativeGgpo->callbackStalls; ++activeNativeGgpo->syncFailures; return false; }
    MixNativeState(*activeNativeGgpo, activeNativeGgpo->simulationFrame, inputs, sizeof(inputs));
    ++activeNativeGgpo->advances;
    const auto advanced = ggpo_advance_frame(activeNativeGgpo->session);
    RecordNativeGgpoError(*activeNativeGgpo, advanced);
    return advanced == GGPO_OK;
}
bool __cdecl NativeGgpoEvent(GGPOEvent* event) {
    if (activeNativeGgpo && event && event->code == GGPO_EVENTCODE_RUNNING) activeNativeGgpo->running = true;
    if (activeNativeGgpo && event && event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) {
        ++activeNativeGgpo->disconnectedEvents;
        ++activeNativeGgpo->fatalErrors;
    }
    return true;
}

GGPOSessionCallbacks NativeGgpoCallbacks() {
    GGPOSessionCallbacks callbacks{};
    callbacks.begin_game = NativeGgpoBegin; callbacks.save_game_state = NativeGgpoSave;
    callbacks.load_game_state = NativeGgpoLoad; callbacks.log_game_state = NativeGgpoLog;
    callbacks.free_buffer = NativeGgpoFree; callbacks.advance_frame = NativeGgpoAdvance;
    callbacks.on_event = NativeGgpoEvent;
    return callbacks;
}

void SampleNativePing(NativeGgpoContext& context) {
    if (!context.session || context.remote == GGPO_INVALID_HANDLE) return;
    GGPONetworkStats stats{};
    activeNativeGgpo = &context;
    if (ggpo_get_network_stats(context.session, context.remote, &stats) == GGPO_OK && stats.network.ping >= 0)
        context.pingUs.push_back(static_cast<std::uint64_t>(stats.network.ping) * 1000ULL);
}

class GameHelperProxy {
public:
    GameHelperProxy() = default;
    ~GameHelperProxy() { Stop(); }
    GameHelperProxy(const GameHelperProxy&) = delete;
    bool Start(std::uint16_t gamePort, std::uint32_t seed, int delayMs, int dropEvery) {
        gamePort_ = gamePort; seed_ = seed; delayMs_ = std::max(0, delayMs); dropEvery_ = dropEvery;
        helperSocket_ = MakeSocket(helperPort_);
        bridgeSocket_ = MakeSocket(bridgePort_);
        if (helperSocket_ == INVALID_SOCKET || bridgeSocket_ == INVALID_SOCKET) { Stop(); return false; }
        stopping_ = false; quiesced_ = false; worker_ = std::thread([this] { Run(); });
        return true;
    }
    bool SetHelperVirtual(std::uint16_t port) { helperVirtual_ = port; return port != 0; }
    std::uint16_t HelperPort() const { return helperPort_; }
    std::uint16_t BridgePort() const { return bridgePort_; }
    std::uint64_t Received() const { return received_.load(); }
    std::uint64_t Forwarded() const { return forwarded_.load(); }
    std::uint64_t Dropped() const { return dropped_.load(); }
    std::uint64_t Unsent() const { return unsent_.load(); }
    std::uint64_t SendErrors() const { return sendErrors_.load(); }
    std::uint64_t UnreadDropped() const { return unreadDropped_.load(); }
    bool Quiesced() const { return quiesced_.load(); }
    std::uint64_t ScheduleDigest() const { return scheduleDigest_; }
    std::uint64_t DirectionScheduleDigest(unsigned direction) const { return scheduleDigestByDirection_[direction & 1u]; }
    std::uint64_t DirectionReceived(unsigned direction) const { return receivedByDirection_[direction & 1u].load(); }
    std::uint64_t DirectionForwarded(unsigned direction) const { return forwardedByDirection_[direction & 1u].load(); }
    std::uint64_t DirectionDropped(unsigned direction) const { return droppedByDirection_[direction & 1u].load(); }
    std::uint64_t DirectionReceivedBytes(unsigned direction) const { return receivedBytes_[direction & 1u].load(); }
    std::uint64_t DirectionForwardedBytes(unsigned direction) const { return forwardedBytes_[direction & 1u].load(); }
    bool Settled() const { return Received() == Forwarded() + Dropped() + SendErrors(); }
    bool ArmWork() {
        armRequested_ = true;
        const auto deadline = GetTickCount64() + 5000;
        while (!impairmentArmed_ && GetTickCount64() < deadline) Sleep(1);
        return impairmentArmed_;
    }
    // Start both directions' drains before joining either worker. Keep the
    // loopback sockets alive until EndMatch has acknowledged helper retirement.
    void RequestDrain() { stopping_ = true; }
    void AwaitDrain() { if (worker_.joinable()) worker_.join(); }
    void Stop() {
        if (helperSocket_ == INVALID_SOCKET && bridgeSocket_ == INVALID_SOCKET) return;
        RequestDrain();
        AwaitDrain();
        if (helperSocket_ != INVALID_SOCKET) closesocket(helperSocket_);
        if (bridgeSocket_ != INVALID_SOCKET) closesocket(bridgeSocket_);
        helperSocket_ = bridgeSocket_ = INVALID_SOCKET;
    }
private:
    struct Pending {
        std::chrono::steady_clock::time_point due;
        SOCKET socket = INVALID_SOCKET;
        sockaddr_in target{};
        std::vector<char> bytes;
        unsigned direction = 0;
    };
    static SOCKET MakeSocket(std::uint16_t& port) {
        const auto socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketValue == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socketValue, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(socketValue); return INVALID_SOCKET; }
        int size = sizeof(address);
        if (getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &size) != 0) { closesocket(socketValue); return INVALID_SOCKET; }
        port = ntohs(address.sin_port);
        u_long nonblocking = 1;
        if (ioctlsocket(socketValue, FIONBIO, &nonblocking) != 0) { closesocket(socketValue); return INVALID_SOCKET; }
        return socketValue;
    }
    bool Schedule(SOCKET socketValue, std::uint16_t targetPort, const char* bytes, int length, unsigned direction) {
        if (!helperVirtual_ || !targetPort || length <= 0) { ++dropped_; return false; }
        const bool armed = impairmentArmed_;
        const auto sequence = armed ? ++directionSequence_[direction] : 0;
        // Direction and logical sequence, rather than arrival order, choose
        // loss. Both helpers therefore see the same seeded impairment trace.
        const auto key = static_cast<std::uint64_t>(seed_) ^ (direction ? 0x9e3779b97f4a7c15ULL : 0x243f6a8885a308d3ULL);
        // Synchronization is unmodified. The worker arms each direction at
        // the prework barrier so variable handshake traffic cannot offset the
        // measured loss pattern. A unit stride preserves every-N semantics.
        const bool drop = armed && dropEvery_ > 0 &&
            ((sequence + key % static_cast<std::uint64_t>(dropEvery_)) % static_cast<std::uint64_t>(dropEvery_) == 0);
        if (armed) {
            scheduleDigestByDirection_[direction] = (scheduleDigestByDirection_[direction] * 1099511628211ULL) ^ (key + sequence + (drop ? 1 : 0));
            scheduleDigest_ = (scheduleDigest_ * 1099511628211ULL) ^ (scheduleDigestByDirection_[direction] + direction);
        }
        if (drop) { ++dropped_; ++droppedByDirection_[direction]; return false; }
        Pending pending;
        pending.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(armed ? delayMs_ : 0);
        pending.socket = socketValue;
        pending.direction = direction;
        pending.target.sin_family = AF_INET; pending.target.sin_addr.s_addr = htonl(INADDR_LOOPBACK); pending.target.sin_port = htons(targetPort);
        pending.bytes.assign(bytes, bytes + length); pending_.push_back(std::move(pending));
        return true;
    }
    void Receive(SOCKET socketValue, std::uint16_t expectedSource, unsigned direction, SOCKET outboundSocket, std::uint16_t targetPort) {
        sockaddr_in source{}; int sourceSize = sizeof(source); std::array<char, 2048> buffer{};
        const auto length = recvfrom(socketValue, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceSize);
        if (length <= 0) return;
        if (expectedSource && ntohs(source.sin_port) != expectedSource) { ++dropped_; return; }
        ++received_;
        ++receivedByDirection_[direction];
        receivedBytes_[direction] += static_cast<std::uint64_t>(length);
        Schedule(outboundSocket, targetPort, buffer.data(), length, direction);
    }
    void DrainSocket(SOCKET socketValue) {
        std::array<char, 2048> buffer{}; sockaddr_in source{}; int sourceSize = sizeof(source);
        while (recvfrom(socketValue, buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceSize) > 0) {
            ++unreadDropped_; ++dropped_;
            sourceSize = sizeof(source);
        }
    }
    void Run() {
        auto drainDeadline = std::chrono::steady_clock::time_point::max();
        auto lastTraffic = std::chrono::steady_clock::now();
        std::uint64_t previousReceived = 0;
        while (!stopping_ || !pending_.empty() || std::chrono::steady_clock::now() < drainDeadline) {
            if (armRequested_ && pending_.empty()) {
                directionSequence_[0] = directionSequence_[1] = 0;
                scheduleDigestByDirection_[0] = scheduleDigestByDirection_[1] = 1469598103934665603ULL;
                scheduleDigest_ = 1469598103934665603ULL;
                armRequested_ = false;
                impairmentArmed_ = true;
            }
            if (stopping_ && drainDeadline == std::chrono::steady_clock::time_point::max())
                drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (!stopping_ || std::chrono::steady_clock::now() < drainDeadline) {
                Receive(bridgeSocket_, gamePort_, 0, helperSocket_, helperVirtual_);
                Receive(helperSocket_, helperVirtual_, 1, bridgeSocket_, gamePort_);
            }
            const auto now = std::chrono::steady_clock::now();
            while (!pending_.empty() && pending_.front().due <= now) {
                auto pending = std::move(pending_.front()); pending_.pop_front();
                const auto sent = sendto(pending.socket, pending.bytes.data(), static_cast<int>(pending.bytes.size()), 0,
                    reinterpret_cast<const sockaddr*>(&pending.target), sizeof(pending.target));
                if (sent == static_cast<int>(pending.bytes.size())) {
                    ++forwarded_; ++forwardedByDirection_[pending.direction];
                    forwardedBytes_[pending.direction] += static_cast<std::uint64_t>(sent);
                    lastTraffic = now;
                }
                else { ++sendErrors_; }
            }
            if (received_.load() != previousReceived) { previousReceived = received_.load(); lastTraffic = now; }
            if (stopping_ && pending_.empty() && now - lastTraffic >= std::chrono::milliseconds(500)) break;
            if (stopping_ && std::chrono::steady_clock::now() >= drainDeadline) {
                DrainSocket(bridgeSocket_); DrainSocket(helperSocket_);
                unreadDropped_ += pending_.size();
                unsent_ += pending_.size(); pending_.clear();
                break;
            }
            Sleep(1);
        }
        if (!pending_.empty()) { unsent_ += pending_.size(); pending_.clear(); }
        quiesced_ = true;
    }
    SOCKET helperSocket_ = INVALID_SOCKET, bridgeSocket_ = INVALID_SOCKET;
    std::thread worker_;
    std::atomic<bool> stopping_{false}, quiesced_{false};
    std::atomic<bool> armRequested_{false}, impairmentArmed_{false};
    std::uint16_t gamePort_ = 0, helperPort_ = 0, bridgePort_ = 0;
    std::atomic<std::uint16_t> helperVirtual_{0};
    std::uint32_t seed_ = 0; int delayMs_ = 0, dropEvery_ = 0;
    std::uint64_t directionSequence_[2]{};
    std::uint64_t scheduleDigestByDirection_[2]{1469598103934665603ULL, 1469598103934665603ULL};
    std::uint64_t scheduleDigest_ = 1469598103934665603ULL;
    std::deque<Pending> pending_;
    std::atomic<std::uint64_t> received_{0}, forwarded_{0}, dropped_{0}, unsent_{0}, sendErrors_{0}, unreadDropped_{0};
    std::atomic<std::uint64_t> receivedByDirection_[2]{}, forwardedByDirection_[2]{}, droppedByDirection_[2]{};
    std::atomic<std::uint64_t> receivedBytes_[2]{}, forwardedBytes_[2]{};
};

struct NativeRun {
    platform::HelperProcess hostProcess;
    platform::HelperProcess guestProcess;
    platform::HelperClient host;
    platform::HelperClient guest;
    std::string hostId;
    std::string guestId;
    std::string invitation;
    json room = nullptr;
    bool hostConnected = false;
    bool guestConnected = false;
    std::uint64_t epoch = 1;
    std::uint16_t hostVirtual = 0;
    std::uint16_t guestVirtual = 0;
    json hostStats = nullptr;
    json guestStats = nullptr;
    json hostStatsBefore = nullptr;
    json guestStatsBefore = nullptr;
    std::string hostGameRoute;
    std::string guestGameRoute;
    std::uint64_t hostStatsEvents = 0;
    std::uint64_t guestStatsEvents = 0;
    std::uint64_t hostStatsEventsBefore = 0;
    std::uint64_t guestStatsEventsBefore = 0;
    std::uint64_t hostStatsEventsAtEnd = 0;
    std::uint64_t guestStatsEventsAtEnd = 0;
    std::vector<std::string> hostGameRoutes;
    std::vector<std::string> guestGameRoutes;
    std::vector<std::string> errors;
    bool workloadActive = false;
    bool measurementStarted = false;
    bool endingRequested = false;
    bool statsFrozen = false;
    bool disconnectedDuringWork = false;
    std::string error;

    bool Start(const std::wstring& executable, bool relayOnly) {
        if (!hostProcess.Start(executable, GetCurrentProcessId(), relayOnly) ||
            !guestProcess.Start(executable, GetCurrentProcessId(), relayOnly)) {
            error = "helper_process_start"; return false;
        }
        if (!host.Start(hostProcess.Bootstrap()) || !guest.Start(guestProcess.Bootstrap())) {
            error = "helper_ipc_start"; return false;
        }
        const auto deadline = GetTickCount64() + 10000;
        while ((host.State() != platform::HelperState::Connected || guest.State() != platform::HelperState::Connected) &&
            GetTickCount64() < deadline) Sleep(2);
        if (host.State() != platform::HelperState::Connected || guest.State() != platform::HelperState::Connected) {
            error = "helper_ipc_connect_timeout"; return false;
        }
        return true;
    }

    void Observe(const json& event, bool isHost) {
        const auto type = event.value("type", std::string());
        if (type == "status") {
            auto& identity = isHost ? hostId : guestId;
            identity = event.value("endpoint", std::string());
        } else if (type == "hosted" && isHost) {
            invitation = event.value("invitation", std::string());
            room = event.contains("room") ? event.at("room") : json(nullptr);
        } else if (type == "connected") {
            const auto peer = event.value("peer", std::string());
            if (isHost && peer == guestId) hostConnected = true;
            if (!isHost && peer == hostId) guestConnected = true;
        } else if (type == "probe_result") {
            // Probe results are a separate connection and are not the route
            // used by the authorized gameplay bridge.
        } else if (type == "game_ready") {
            const auto peer = event.value("peer", std::string());
            const auto generation = event.value("generation", std::uint64_t(0));
            if (generation == 1) {
                if (isHost && peer == guestId) hostVirtual = event.value("virtual_port", 0u);
                if (!isHost && peer == hostId) guestVirtual = event.value("virtual_port", 0u);
                const auto routeValue = event.value("route", std::string());
                if (isHost && !routeValue.empty()) { hostGameRoute = routeValue; hostGameRoutes.push_back(routeValue); }
                if (!isHost && !routeValue.empty()) { guestGameRoute = routeValue; guestGameRoutes.push_back(routeValue); }
            }
        } else if (type == "game_closed" && event.value("generation", std::uint64_t(0)) == 1) {
            if (measurementStarted && !endingRequested) disconnectedDuringWork = true;
            if (isHost) hostVirtual = 0; else guestVirtual = 0;
        } else if (type == "disconnected" || type == "connection_closed") {
            if (measurementStarted && !endingRequested) disconnectedDuringWork = true;
        } else if (type == "statistics" && event.value("generation", std::uint64_t(0)) == 1) {
            const auto peer = event.value("peer", std::string());
            const auto expected = isHost ? guestId : hostId;
            if (peer == expected) {
                if (statsFrozen) {
                    const auto& certified = isHost ? hostStats : guestStats;
                    for (const auto* field : {"sent_packets", "received_packets", "sent_bytes", "received_bytes",
                        "rejected_packets", "congestion_events", "local_drops"}) {
                        if (certified.at(field) != event.at(field)) {
                            error = "statistics_changed_after_drain"; errors.push_back(error); break;
                        }
                    }
                    return;
                }
                (isHost ? hostStats : guestStats) = event;
                if (isHost) ++hostStatsEvents; else ++guestStatsEvents;
                const auto route = event.value("route", std::string());
                if (isHost) { hostGameRoute = route; hostGameRoutes.push_back(route); }
                else { guestGameRoute = route; guestGameRoutes.push_back(route); }
            }
        } else if (type == "error") {
            const auto code = event.value("code", std::string("helper_error"));
            errors.push_back(code);
            error = code;
        }
    }

    void BeginWork() {
        hostStatsBefore = hostStats; guestStatsBefore = guestStats;
        hostStatsEventsBefore = hostStatsEvents; guestStatsEventsBefore = guestStatsEvents;
        hostGameRoutes = {hostGameRoute}; guestGameRoutes = {guestGameRoute};
        workloadActive = true; measurementStarted = true; disconnectedDuringWork = false;
    }

    void EndWork() {
        Pump();
        hostStatsEventsAtEnd = hostStatsEvents; guestStatsEventsAtEnd = guestStatsEvents;
        workloadActive = false;
    }

    static std::uint64_t Counter(const json& value, const char* field) {
        return value.is_object() && value.contains(field) && value.at(field).is_number_unsigned()
            ? value.at(field).get<std::uint64_t>() : 0;
    }

    static json CounterDeltas(const json& before, const json& after) {
        json delta = json::object();
        for (const auto* field : {"sent_packets", "received_packets", "sent_bytes", "received_bytes",
            "rejected_packets", "congestion_events", "local_drops"}) {
            const auto a = Counter(after, field), b = Counter(before, field);
            delta[field] = a >= b ? json(a - b) : json(nullptr);
        }
        return delta;
    }

    static bool MatchesProxy(const json& stats, const GameHelperProxy& proxy) {
        if (!stats.is_object()) return false;
        for (const auto* field : {"sent_packets", "received_packets", "sent_bytes", "received_bytes",
            "rejected_packets", "congestion_events", "local_drops"}) {
            if (!stats.contains(field) || !stats.at(field).is_number_unsigned()) return false;
        }
        return Counter(stats, "sent_packets") == proxy.DirectionForwarded(0) &&
            Counter(stats, "received_packets") == proxy.DirectionReceived(1) &&
            Counter(stats, "sent_bytes") == proxy.DirectionForwardedBytes(0) &&
            Counter(stats, "received_bytes") == proxy.DirectionReceivedBytes(1) &&
            Counter(stats, "rejected_packets") == 0 && Counter(stats, "local_drops") == 0 &&
            Counter(stats, "congestion_events") == 0;
    }

    void Pump() {
        platform::HelperMessage message;
        while (host.TryReceive(message)) {
            try { Observe(json::parse(message.payload), true); } catch (...) {
                error = "invalid_host_event"; errors.push_back(error);
            }
        }
        while (guest.TryReceive(message)) {
            try { Observe(json::parse(message.payload), false); } catch (...) {
                error = "invalid_guest_event"; errors.push_back(error);
            }
        }
    }

    template <typename Predicate>
    bool Wait(Predicate done, int timeoutMs) {
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
        do {
            Pump();
            if (done()) return true;
            Sleep(2);
        } while (GetTickCount64() < deadline);
        Pump();
        return done();
    }

    bool Send(platform::HelperClient& client, const json& command) {
        return client.Send(command.dump());
    }

    bool OpenRoom(int timeoutMs) {
        if (!Send(host, json{{"type", "status"}}) || !Send(guest, json{{"type", "status"}})) {
            error = "status_queue"; return false;
        }
        if (!Wait([&] { return hostId.size() == 64 && guestId.size() == 64; }, timeoutMs)) {
            error = error.empty() ? "status_timeout" : error; return false;
        }
        if (!Send(host, json{{"type", "host"}, {"epoch", epoch}, {"build", "recovery-benchmark"}})) {
            error = "host_queue"; return false;
        }
        if (!Wait([&] { return !invitation.empty() && !room.is_null(); }, timeoutMs)) {
            error = error.empty() ? "host_timeout" : error; return false;
        }
        if (!Send(guest, json{{"type", "join"}, {"epoch", epoch}, {"invitation", invitation}, {"build", "recovery-benchmark"}})) {
            error = "join_queue"; return false;
        }
        return Wait([&] {
            return error.empty() && hostConnected && guestConnected;
        }, timeoutMs);
    }
};

json LatencyJson(std::vector<std::uint64_t> values) {
    if (values.empty()) return nullptr;
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) {
        const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
        return values[index];
    };
    return json{{"samples", values.size()}, {"p50_us", percentile(.50)}, {"p95_us", percentile(.95)},
        {"p99_us", percentile(.99)}, {"max_us", values.back()}};
}

std::string RouteClass(const std::string& route) {
    if (route.rfind("ip:[", 0) == 0) {
        const auto close = route.find(']');
        if (close != std::string::npos) return route.substr(0, close + 1);
    }
    const auto lastColon = route.rfind(':');
    if (lastColon != std::string::npos) {
        const auto port = route.substr(lastColon + 1);
        if (!port.empty() && std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return route.substr(0, lastColon);
    }
    return route;
}

struct GgpoContext {
    GGPOSession* session = nullptr;
    GGPOPlayerHandle local = GGPO_INVALID_HANDLE;
    int currentFrame = 0;
    std::uint64_t saves = 0;
    std::uint64_t loads = 0;
    std::uint64_t predictionStalls = 0;
    std::uint64_t notSynchronized = 0;
    std::uint64_t inputDropped = 0;
    std::uint64_t fatalErrors = 0;
    std::uint64_t advances = 0;
    std::uint64_t acceptedFrames = 0;
    int maxReplayDepth = 0;
    bool inputSubmitted = false;
    int initialChecksum = 0;
    int finalChecksum = 0;
    bool hasChecksum = false;
    bool running = false;
    sf4e::pacing::PacingController* pacer = nullptr;
};

// Per thread: the rift mode runs one session on each of two threads.
thread_local GgpoContext* activeGgpo = nullptr;
void RecordGgpoError(GgpoContext& context, GGPOErrorCode code) {
    if (code == GGPO_OK) return;
    if (code == GGPO_ERRORCODE_PREDICTION_THRESHOLD) ++context.predictionStalls;
    else if (code == GGPO_ERRORCODE_NOT_SYNCHRONIZED) ++context.notSynchronized;
    else if (code == GGPO_ERRORCODE_INPUT_DROPPED) ++context.inputDropped;
    else ++context.fatalErrors;
}
bool __cdecl GgpoBegin(const char*) { return true; }
bool __cdecl GgpoSave(unsigned char** buffer, int* length, int* checksum, int frame) {
    if (!activeGgpo || !buffer || !length || !checksum) return false;
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(frame)));
    if (!*buffer) return false;
    std::memcpy(*buffer, &frame, sizeof(frame));
    *length = sizeof(frame); *checksum = frame;
    if (!activeGgpo->hasChecksum) {
        activeGgpo->initialChecksum = frame;
        activeGgpo->hasChecksum = true;
    }
    activeGgpo->finalChecksum = frame;
    ++activeGgpo->saves; return true;
}
bool __cdecl GgpoLoad(unsigned char* buffer, int length) {
    if (!activeGgpo || !buffer || length != static_cast<int>(sizeof(int))) return false;
    int saved = 0; std::memcpy(&saved, buffer, sizeof(saved));
    ++activeGgpo->loads;
    activeGgpo->maxReplayDepth = std::max(activeGgpo->maxReplayDepth,
        std::max(0, activeGgpo->currentFrame - saved));
    return true;
}
bool __cdecl GgpoLog(char*, unsigned char*, int) { return true; }
void __cdecl GgpoFree(void* buffer) { std::free(buffer); }
bool __cdecl GgpoAdvance(int) {
    if (!activeGgpo || !activeGgpo->session) return false;
    unsigned char inputs[2]{}; int disconnected = 0;
    const auto sync = ggpo_synchronize_input(activeGgpo->session, inputs, sizeof(inputs), &disconnected);
    if (sync != GGPO_OK) {
        RecordGgpoError(*activeGgpo, sync);
        return false;
    }
    ++activeGgpo->advances;
    const auto advanced = ggpo_advance_frame(activeGgpo->session);
    RecordGgpoError(*activeGgpo, advanced);
    return advanced == GGPO_OK;
}
bool __cdecl GgpoEvent(GGPOEvent* event) {
    if (activeGgpo && event && event->code == GGPO_EVENTCODE_RUNNING) activeGgpo->running = true;
    // Set in rift runs; the controller ignores the event in continuous mode.
    if (activeGgpo && event && event->code == GGPO_EVENTCODE_TIMESYNC && activeGgpo->pacer)
        activeGgpo->pacer->OnRecommendation(event->u.timesync.frames_ahead);
    return true;
}

unsigned short ReservePort() {
    SOCKET socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketValue == INVALID_SOCKET) return 0;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socketValue, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(socketValue); return 0; }
    int size = sizeof(address); getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &size);
    const auto port = ntohs(address.sin_port); closesocket(socketValue); return port;
}

class UdpImpairmentProxy {
public:
    ~UdpImpairmentProxy() { Stop(); }
    bool Start(std::uint16_t firstPort, std::uint16_t secondPort, std::uint32_t seed, int delayMs, int dropEvery,
        int jitterMs = 0, int burstMs = 0, int burstEveryMs = 0) {
        jitterMs_ = std::max(0, jitterMs); burstMs_ = std::max(0, burstMs); burstEveryMs_ = std::max(0, burstEveryMs);
        if (burstMs_ > 0 && burstEveryMs_ <= burstMs_) return false; // a burst needs a longer period
        started_ = std::chrono::steady_clock::now();
        firstPort_ = firstPort; secondPort_ = secondPort; seed_ = seed;
        delayMs_ = std::max(0, delayMs); dropEvery_ = dropEvery;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) return false;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(socket_); socket_ = INVALID_SOCKET; return false; }
        int size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) { closesocket(socket_); socket_ = INVALID_SOCKET; return false; }
        port_ = ntohs(address.sin_port);
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) { closesocket(socket_); socket_ = INVALID_SOCKET; return false; }
        stopping_ = false; worker_ = std::thread([this] { Run(); });
        return true;
    }
    std::uint16_t Port() const { return port_; }
    std::uint64_t Received() const { return received_.load(); }
    std::uint64_t Forwarded() const { return forwarded_.load(); }
    std::uint64_t Dropped() const { return dropped_.load(); }
    std::uint64_t Unsent() const { return unsent_.load(); }
    std::uint64_t ScheduleDigest() const { return scheduleDigest_; }
    bool Quiesced() const { return quiesced_.load(); }
    void Stop() {
        if (socket_ == INVALID_SOCKET) return;
        stopping_ = true;
        if (worker_.joinable()) worker_.join();
        closesocket(socket_); socket_ = INVALID_SOCKET;
    }
private:
    struct Pending {
        std::chrono::steady_clock::time_point due;
        sockaddr_in target{};
        std::vector<char> bytes;
    };
    void Run() {
        while (!stopping_ || !pending_.empty()) {
            sockaddr_in source{}; int sourceSize = sizeof(source);
            std::array<char, 2048> buffer{};
            if (!stopping_) {
                const int length = recvfrom(socket_, buffer.data(), static_cast<int>(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), &sourceSize);
                if (length > 0) {
                    const auto sourcePort = ntohs(source.sin_port);
                    const unsigned direction = sourcePort == firstPort_ ? 0u : sourcePort == secondPort_ ? 1u : 2u;
                    if (direction > 1u) ++dropped_;
                    else {
                        ++received_;
                        const auto sequence = ++directionSequence_[direction];
                        const auto key = static_cast<std::uint64_t>(seed_) ^
                            (direction ? 0x9e3779b97f4a7c15ULL : 0x243f6a8885a308d3ULL);
                        // A burst drops both directions for burstMs out of every burstEveryMs.
                        const auto sinceStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_).count();
                        const bool burst = burstMs_ > 0 && sequence > warmupPackets_ &&
                            sinceStartMs % burstEveryMs_ >= burstEveryMs_ - burstMs_;
                        const bool drop = burst || (dropEvery_ > 0 && sequence > warmupPackets_ &&
                            ((key + sequence * 0x9e3779b9ULL) % static_cast<std::uint64_t>(dropEvery_) == 0));
                        scheduleDigest_ = (scheduleDigest_ * 1099511628211ULL) ^ (key + sequence + (drop ? 1 : 0));
                        if (drop) ++dropped_;
                        else {
                            Pending pending;
                            // Deterministic per-packet jitter in [0, jitterMs]; it may reorder.
                            const int jitter = jitterMs_ > 0 ? static_cast<int>(
                                ((key ^ (sequence * 0xd6e8feb86659fd93ULL)) >> 17) % static_cast<std::uint64_t>(jitterMs_ + 1)) : 0;
                            pending.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs_ + jitter);
                            pending.target.sin_family = AF_INET;
                            pending.target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                            pending.target.sin_port = htons(direction == 0 ? secondPort_ : firstPort_);
                            pending.bytes.assign(buffer.data(), buffer.data() + length); pending_.push_back(std::move(pending));
                        }
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            // With jitter the queue is not ordered by due time, so scan all of it.
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->due > now) { ++it; continue; }
                if (sendto(socket_, it->bytes.data(), static_cast<int>(it->bytes.size()), 0,
                    reinterpret_cast<sockaddr*>(&it->target), sizeof(it->target)) >= 0) ++forwarded_;
                it = pending_.erase(it);
            }
            Sleep(1);
        }
        unsent_ += pending_.size(); pending_.clear(); quiesced_ = true;
    }
    SOCKET socket_ = INVALID_SOCKET;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::deque<Pending> pending_;
    std::uint16_t firstPort_ = 0, secondPort_ = 0, port_ = 0;
    std::uint32_t seed_ = 0;
    int delayMs_ = 0, dropEvery_ = 0, jitterMs_ = 0, burstMs_ = 0, burstEveryMs_ = 0;
    std::chrono::steady_clock::time_point started_{};
    static constexpr std::uint64_t warmupPackets_ = 16;
    std::uint64_t directionSequence_[2]{};
    std::uint64_t scheduleDigest_ = 1469598103934665603ULL;
    std::atomic<bool> quiesced_{false};
    std::atomic<std::uint64_t> received_{0}, forwarded_{0}, dropped_{0}, unsent_{0};
};

json RunSyntheticGgpo(const Options& options) {
    json result{{"measured", false}, {"synthetic_ggpo", true}, {"native_gameplay", false},
        {"schedule", json{{"seed", options.seed}, {"frames", options.frames}, {"application_delay_ms", options.delayMs},
            {"application_drop_every", options.dropEvery}, {"packet_proxy", true}}}};
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) { result["error"] = "winsock"; return result; }
    GGPOSessionCallbacks callbacks{};
    callbacks.begin_game = GgpoBegin; callbacks.save_game_state = GgpoSave; callbacks.load_game_state = GgpoLoad;
    callbacks.log_game_state = GgpoLog; callbacks.free_buffer = GgpoFree; callbacks.advance_frame = GgpoAdvance; callbacks.on_event = GgpoEvent;
    GgpoContext first, second;
    ULONGLONG benchmarkDeadline = 0;
    std::chrono::steady_clock::time_point nextTick{};
    std::uint64_t fatalErrors = 0;
    bool frameComplete = false, proxyMeasured = false;
    const auto firstPort = ReservePort(); auto secondPort = ReservePort();
    if (!firstPort || !secondPort) { result["error"] = "ggpo_port"; WSACleanup(); return result; }
    while (firstPort == secondPort) secondPort = ReservePort();
    UdpImpairmentProxy proxy;
    if (!proxy.Start(firstPort, secondPort, options.seed, options.delayMs, options.dropEvery)) {
        result["error"] = "ggpo_proxy"; WSACleanup(); return result;
    }
    auto firstResult = ggpo_start_session(&first.session, &callbacks, "recovery-benchmark", 2, 1, firstPort);
    auto secondResult = ggpo_start_session(&second.session, &callbacks, "recovery-benchmark", 2, 1, secondPort);
    if (firstResult != GGPO_OK || secondResult != GGPO_OK) {
        result["error"] = "ggpo_start"; if (first.session) ggpo_close_session(first.session); if (second.session) ggpo_close_session(second.session); WSACleanup(); return result;
    }
    GGPOPlayer player{}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = 1;
    if (ggpo_add_player(first.session, &player, &first.local) != GGPO_OK) { result["error"] = "ggpo_player"; goto cleanup; }
    player.player_num = 2; if (ggpo_add_player(second.session, &player, &second.local) != GGPO_OK) { result["error"] = "ggpo_player"; goto cleanup; }
    player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1"); player.u.remote.port = proxy.Port();
    { GGPOPlayerHandle unused{}; if (ggpo_add_player(first.session, &player, &unused) != GGPO_OK) { result["error"] = "ggpo_player"; goto cleanup; } }
    player.player_num = 1; player.u.remote.port = proxy.Port();
    { GGPOPlayerHandle unused{}; if (ggpo_add_player(second.session, &player, &unused) != GGPO_OK) { result["error"] = "ggpo_player"; goto cleanup; } }
    for (int spin = 0; spin < 5000 && (!first.running || !second.running); ++spin) {
        activeGgpo = &first; ggpo_idle(first.session, 0); activeGgpo = &second; ggpo_idle(second.session, 0); Sleep(1);
    }
    if (!first.running || !second.running) { result["error"] = "ggpo_running_timeout"; goto cleanup; }
    benchmarkDeadline = GetTickCount64() + static_cast<ULONGLONG>(options.timeoutMs);
    nextTick = std::chrono::steady_clock::now();
    while ((first.acceptedFrames < static_cast<std::uint64_t>(options.frames) ||
        second.acceptedFrames < static_cast<std::uint64_t>(options.frames)) &&
        GetTickCount64() < benchmarkDeadline) {
        activeGgpo = &first; RecordGgpoError(first, ggpo_idle(first.session, 0));
        activeGgpo = &second; RecordGgpoError(second, ggpo_idle(second.session, 0));
        const auto addInput = [&](GgpoContext& context, int player) {
            if (context.inputSubmitted || context.acceptedFrames >= static_cast<std::uint64_t>(options.frames)) return;
            unsigned char input = static_cast<unsigned char>((options.seed +
                static_cast<std::uint32_t>(context.currentFrame) * (player == 1 ? 17u : 31u) +
                static_cast<std::uint32_t>(player * 13)) & 255u);
            activeGgpo = &context;
            const auto code = ggpo_add_local_input(context.session, context.local, &input, 1);
            RecordGgpoError(context, code);
            if (code == GGPO_OK) context.inputSubmitted = true;
        };
        addInput(first, 1); addInput(second, 2);
        const auto advance = [&](GgpoContext& context) {
            if (!context.inputSubmitted || context.acceptedFrames >= static_cast<std::uint64_t>(options.frames)) return;
            unsigned char inputs[2]{}; int disconnected = 0;
            activeGgpo = &context;
            const auto sync = ggpo_synchronize_input(context.session, inputs, sizeof(inputs), &disconnected);
            RecordGgpoError(context, sync);
            if (sync != GGPO_OK) return;
            const auto code = ggpo_advance_frame(context.session);
            RecordGgpoError(context, code);
            if (code == GGPO_OK) {
                ++context.acceptedFrames; ++context.currentFrame; context.inputSubmitted = false;
            }
        };
        advance(first); advance(second);
        nextTick += std::chrono::milliseconds(16);
        const auto now = std::chrono::steady_clock::now();
        if (now < nextTick) std::this_thread::sleep_for(nextTick - now);
        else nextTick = now;
    }
    proxy.Stop();
    fatalErrors = first.fatalErrors + second.fatalErrors;
    frameComplete = first.acceptedFrames >= static_cast<std::uint64_t>(options.frames) &&
        second.acceptedFrames >= static_cast<std::uint64_t>(options.frames);
    proxyMeasured = proxy.Quiesced() && proxy.Unsent() == 0 && proxy.Received() != 0;
    result["measured"] = frameComplete && fatalErrors == 0 && proxyMeasured;
    result["phase_status"] = frameComplete && fatalErrors == 0 ?
        (proxyMeasured ? "complete" : "completed_with_unmeasured_fields") : "completed_with_errors";
    result["frames_requested"] = options.frames;
    result["frames_advanced"] = first.acceptedFrames + second.acceptedFrames;
    result["accepted_frame_counts"] = json{{"first", first.acceptedFrames}, {"second", second.acceptedFrames},
        {"target_hz", 60}, {"pacing_interval_us", 16667}};
    result["prediction_stalls"] = first.predictionStalls + second.predictionStalls;
    result["not_synchronized"] = first.notSynchronized + second.notSynchronized;
    result["input_dropped"] = first.inputDropped + second.inputDropped;
    result["replay_loads"] = first.loads + second.loads;
    result["max_replay_depth"] = std::max(first.maxReplayDepth, second.maxReplayDepth);
    result["save_callbacks"] = first.saves + second.saves;
    result["fatal_callback_errors"] = fatalErrors;
    result["state_checksums"] = json{
        {"first_initial", first.hasChecksum ? json(first.initialChecksum) : json(nullptr)},
        {"second_initial", second.hasChecksum ? json(second.initialChecksum) : json(nullptr)},
        {"first_final", first.hasChecksum ? json(first.finalChecksum) : json(nullptr)},
        {"second_final", second.hasChecksum ? json(second.finalChecksum) : json(nullptr)},
        {"initial_equal", first.hasChecksum && second.hasChecksum && first.initialChecksum == second.initialChecksum},
        {"final_equal", first.hasChecksum && second.hasChecksum && first.finalChecksum == second.finalChecksum},
        {"available", first.hasChecksum && second.hasChecksum},
        {"basis", "benchmark save-frame checksum; not an SF4 gameplay state checksum"}};
    result["wire_proxy"] = json{{"measured", proxyMeasured}, {"received", proxy.Received()}, {"forwarded", proxy.Forwarded()},
        {"dropped", proxy.Dropped()}, {"unsent", proxy.Unsent()}, {"quiesced", proxy.Quiesced()},
        {"schedule_digest", proxy.ScheduleDigest()}, {"warmup_unimpaired_packets_per_direction", 16},
        {"delay_ms", options.delayMs}, {"drop_every", options.dropEvery}};
    result["notes"] = "Optional synthetic GGPO callback diagnostic with a deterministic per-direction loopback UDP impairment proxy; no native SF4 gameplay. Native helper metrics are a separate actual selected-route phase.";
cleanup:
    proxy.Stop();
    if (first.session) { activeGgpo = &first; ggpo_close_session(first.session); }
    if (second.session) { activeGgpo = &second; ggpo_close_session(second.session); }
    WSACleanup();
    return result;
}

// Sleeps coarsely, then spins: Sleep alone is too coarse to hold a 60 Hz
// cadence or a 1-3 ms pacing wait.
void PreciseWaitUntil(std::chrono::steady_clock::time_point due) {
    for (;;) {
        const auto left = due - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return;
        if (left > std::chrono::milliseconds(2)) Sleep(1); else YieldProcessor();
    }
}

// Two peers on their own threads and clocks, one running fast, through the
// impairment proxy. Measures how far apart the peers drift and how unevenly the
// rollbacks fall, with the coarse GGPO time sync or the continuous controller.
// Input repair is selected by SF4E_GGPO_INPUT_REPAIR in the environment, as in
// the game.
json RunRift(const Options& options) {
    json result{{"measured", false}, {"mode", options.continuous ? "continuous" : "coarse"}};
    GGPOSessionCallbacks callbacks{};
    callbacks.begin_game = GgpoBegin; callbacks.save_game_state = GgpoSave; callbacks.load_game_state = GgpoLoad;
    callbacks.log_game_state = GgpoLog; callbacks.free_buffer = GgpoFree; callbacks.advance_frame = GgpoAdvance; callbacks.on_event = GgpoEvent;
    const auto firstPort = ReservePort(); auto secondPort = ReservePort();
    while (secondPort && firstPort == secondPort) secondPort = ReservePort();
    UdpImpairmentProxy proxy;
    if (!firstPort || !secondPort || !proxy.Start(firstPort, secondPort, options.seed, options.delayMs,
        options.dropEvery, options.jitterMs, options.burstMs, options.burstEveryMs)) {
        result["error"] = "rift_proxy"; return result;
    }
    struct Peer {
        GgpoContext context;
        sf4e::pacing::PacingController pacer;
        GGPOPlayerHandle remote = GGPO_INVALID_HANDLE;
        std::atomic<int> frame{0};
        std::atomic<bool> ready{false}, failed{false};
        std::uint64_t stallTicks = 0;
        std::vector<double> riftSamples; // own frame minus peer frame, per tick
    } peers[2];
    std::atomic<bool> go{false}, stop{false};
    const auto run = [&](int index) {
        Peer& self = peers[index]; Peer& other = peers[1 - index];
        GgpoContext& context = self.context;
        self.pacer.InitDefaults(); self.pacer.continuous = options.continuous; context.pacer = &self.pacer;
        activeGgpo = &context;
        GGPOPlayer player{}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = index + 1;
        bool ok = ggpo_start_session(&context.session, &callbacks, "rift-benchmark", 2, 1, index ? secondPort : firstPort) == GGPO_OK &&
            ggpo_add_player(context.session, &player, &context.local) == GGPO_OK &&
            ggpo_set_frame_delay(context.session, context.local, options.inputDelay) == GGPO_OK;
        player.type = GGPO_PLAYERTYPE_REMOTE; player.player_num = 2 - index;
        strcpy_s(player.u.remote.ip_address, "127.0.0.1"); player.u.remote.port = proxy.Port();
        ok = ok && ggpo_add_player(context.session, &player, &self.remote) == GGPO_OK;
        for (int spin = 0; ok && spin < 5000 && !context.running; ++spin) { ggpo_idle(context.session, 0); Sleep(1); }
        if (!ok || !context.running) { self.failed = true; self.ready = true; return; }
        self.ready = true;
        while (!go && !stop) { ggpo_idle(context.session, 0); Sleep(1); }
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / (index ? options.fastHz : 60.0)));
        const double periodMs = 1000.0 / (index ? options.fastHz : 60.0);
        auto nextTick = std::chrono::steady_clock::now();
        while (!stop) {
            double shiftMs = 0.0;
            RecordGgpoError(context, ggpo_idle(context.session, 0));
            // Inputs change every 8 frames so only some predictions miss.
            unsigned char input = static_cast<unsigned char>((options.seed + static_cast<std::uint32_t>(
                context.currentFrame / 8) * (index ? 31u : 17u) + static_cast<std::uint32_t>(index)) & 255u);
            const auto added = ggpo_add_local_input(context.session, context.local, &input, 1);
            RecordGgpoError(context, added);
            if (added == GGPO_ERRORCODE_PREDICTION_THRESHOLD) { ++self.stallTicks; self.pacer.OnPredictionStall(); }
            if (added == GGPO_OK) {
                unsigned char inputs[2]{}; int disconnected = 0;
                if (ggpo_synchronize_input(context.session, inputs, sizeof(inputs), &disconnected) == GGPO_OK &&
                    ggpo_advance_frame(context.session) == GGPO_OK) {
                    ++context.acceptedFrames; ++context.currentFrame; self.frame = context.currentFrame;
                }
                // The game's pacing step; like the game it is skipped on a
                // stalled tick.
                GGPONetworkStats stats{};
                if (ggpo_get_network_stats(context.session, self.remote, &stats) == GGPO_OK)
                    self.pacer.OnRiftSample(stats.timesync.local_frames_behind, stats.timesync.remote_frames_behind);
                shiftMs = self.pacer.NextShiftMs();
            }
            self.riftSamples.push_back(static_cast<double>(context.currentFrame - other.frame.load()));
            // The frame deadline plays the game's limiter: a pacing shift moves
            // it for one frame, as fD3D::LimitFrame does.
            const auto frameStart = nextTick, now = std::chrono::steady_clock::now();
            const double shiftedMs = sf4e::pacing::ShiftedPeriodMs(
                periodMs, std::chrono::duration<double, std::milli>(now - frameStart).count(), shiftMs);
            nextTick += shiftMs == 0.0 ? period : std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(shiftedMs));
            if (nextTick < now) nextTick = now;
            PreciseWaitUntil(nextTick);
            self.pacer.OnShiftApplied(sf4e::pacing::AppliedShiftMs(
                periodMs, std::chrono::duration<double, std::milli>(nextTick - frameStart).count(), shiftMs));
        }
    };
    timeBeginPeriod(1);
    std::thread threads[2]{std::thread(run, 0), std::thread(run, 1)};
    const auto setupDeadline = GetTickCount64() + 10000;
    while ((!peers[0].ready || !peers[1].ready) && GetTickCount64() < setupDeadline) Sleep(5);
    const bool started = peers[0].ready && peers[1].ready && !peers[0].failed && !peers[1].failed;
    if (started) { go = true; Sleep(static_cast<DWORD>(options.frames * 1000.0 / 60.0)); }
    stop = true;
    for (auto& thread : threads) thread.join();
    timeEndPeriod(1);
    proxy.Stop();
    const auto peerJson = [&](Peer& peer) {
        // Skip the first two seconds: both peers start in step by construction.
        std::vector<double> magnitudes;
        for (std::size_t i = std::min<std::size_t>(peer.riftSamples.size(), 120); i < peer.riftSamples.size(); ++i)
            magnitudes.push_back(std::abs(peer.riftSamples[i]));
        std::sort(magnitudes.begin(), magnitudes.end());
        const double mean = magnitudes.empty() ? 0.0 :
            std::accumulate(magnitudes.begin(), magnitudes.end(), 0.0) / static_cast<double>(magnitudes.size());
        return json{{"frames", peer.context.acceptedFrames}, {"rollback_frames", peer.context.advances},
            {"rollback_loads", peer.context.loads}, {"max_rollback_depth", peer.context.maxReplayDepth},
            {"prediction_stall_ticks", peer.stallTicks}, {"fatal_errors", peer.context.fatalErrors},
            {"mean_abs_rift_frames", mean},
            {"p95_abs_rift_frames", magnitudes.empty() ? 0.0 : magnitudes[magnitudes.size() * 95 / 100]},
            {"pacing_wait_ms", peer.pacer.msSlowedTotal}, {"pacing_speedup_ms", peer.pacer.msSpedUpTotal},
            {"timesync_recommendations", peer.pacer.recommendationsReceived},
            {"rift_ema_frames", peer.pacer.riftFramesEma}};
    };
    for (auto& peer : peers) if (peer.context.session) { activeGgpo = &peer.context; ggpo_close_session(peer.context.session); }
    result["measured"] = started && !peers[0].context.fatalErrors && !peers[1].context.fatalErrors;
    if (!started) result["error"] = "rift_start";
    result["slow_peer_60hz"] = peerJson(peers[0]);
    result["fast_peer"] = peerJson(peers[1]);
    const auto a = peers[0].context.advances, b = peers[1].context.advances;
    result["rollback_imbalance"] = a + b ? static_cast<double>(a > b ? a - b : b - a) / static_cast<double>(a + b) : 0.0;
    result["schedule"] = json{{"delay_ms", options.delayMs}, {"jitter_ms", options.jitterMs}, {"drop_every", options.dropEvery},
        {"burst_ms", options.burstMs}, {"burst_every_ms", options.burstEveryMs}, {"fast_hz", options.fastHz},
        {"input_delay", options.inputDelay}, {"frames", options.frames}};
    result["wire_proxy"] = json{{"received", proxy.Received()}, {"forwarded", proxy.Forwarded()}, {"dropped", proxy.Dropped()}};
    return result;
}

json RunNativeGgpo(const Options& options) {
    const auto started = std::chrono::steady_clock::now();
    NativeRun run;
    GameHelperProxy hostProxy, guestProxy;
    NativeGgpoContext hostGgpo, guestGgpo;
    GGPOSession* hostSession = nullptr;
    GGPOSession* guestSession = nullptr;
    ResourcePair before{}, after{};
    std::string failure;
    bool completed = false;
    bool statsFresh = false;
    bool statsMatched = false;
    bool confirmed = false;
    int hostConfirmed = -1, guestConfirmed = -1;
    std::uint64_t sampledPeak = 0;
    std::uint64_t resourceSamples = 0;
    std::int64_t workloadUs = 0;
    auto workloadStarted = started;
    bool proxyQuiesced = false;
    bool ggpoCloseOk = true;
    json loadedGgpo = LoadedGgpoArtifact();

    json result{{"measured", false}, {"native_gameplay", false},
        {"transport", "GGPO through selected Iroh helper gameplay bridge"},
        {"helper", NarrowPath(options.helper)}, {"selected_route", nullptr},
        {"route_status", nullptr}, {"error", nullptr}};

    do {
        if (!run.Start(options.helper, options.relayOnly)) { failure = run.error.empty() ? "helper_process_start" : run.error; break; }
        if (!run.OpenRoom(options.timeoutMs)) { failure = run.error.empty() ? "room_open_failed" : run.error; break; }
        const auto hostGamePort = ReservePort();
        auto guestGamePort = ReservePort();
        if (!hostGamePort || !guestGamePort) { failure = "game_port"; break; }
        while (hostGamePort == guestGamePort) guestGamePort = ReservePort();
        if (!hostProxy.Start(hostGamePort, options.seed ^ 0x13579bdfu, options.delayMs, options.dropEvery) ||
            !guestProxy.Start(guestGamePort, options.seed ^ 0x2468ace0u, options.delayMs, options.dropEvery)) {
            failure = "native_proxy_start"; break;
        }

        std::array<std::uint8_t, 32> capability{};
        std::uint32_t random = options.seed;
        for (auto& byte : capability) byte = static_cast<std::uint8_t>(NextRandom(random));
        const auto roomBytes = RoomBytes(run.room);
        const auto hostCommand = json{{"type", "prepare_game"}, {"epoch", run.epoch}, {"peer", run.guestId},
            {"room", roomBytes}, {"generation", 1}, {"capability", capability}, {"local_port", hostProxy.HelperPort()},
            {"max_packet", 1024}, {"dial", false}};
        const auto guestCommand = json{{"type", "prepare_game"}, {"epoch", run.epoch}, {"peer", run.hostId},
            {"room", roomBytes}, {"generation", 1}, {"capability", capability}, {"local_port", guestProxy.HelperPort()},
            {"max_packet", 1024}, {"dial", true}};
        if (!run.Send(run.host, hostCommand) || !run.Send(run.guest, guestCommand) ||
            !run.Wait([&] { return run.hostVirtual != 0 && run.guestVirtual != 0; }, options.timeoutMs)) {
            failure = run.error.empty() ? "game_mapping_timeout" : run.error; break;
        }
        if (!hostProxy.SetHelperVirtual(run.hostVirtual) || !guestProxy.SetHelperVirtual(run.guestVirtual)) {
            failure = "native_proxy_mapping"; break;
        }

        auto callbacks = NativeGgpoCallbacks();
        if (ggpo_start_session(&hostSession, &callbacks, "recovery-benchmark", 2, 1, hostGamePort) != GGPO_OK ||
            ggpo_start_session(&guestSession, &callbacks, "recovery-benchmark", 2, 1, guestGamePort) != GGPO_OK) {
            failure = "ggpo_start"; break;
        }
        hostGgpo.session = hostSession; guestGgpo.session = guestSession;
        GGPOPlayer local{}; local.size = sizeof(local); local.type = GGPO_PLAYERTYPE_LOCAL; local.player_num = 1;
        activeNativeGgpo = &hostGgpo;
        if (ggpo_add_player(hostSession, &local, &hostGgpo.local) != GGPO_OK) { failure = "ggpo_host_player"; break; }
        local.player_num = 2; activeNativeGgpo = &guestGgpo;
        if (ggpo_add_player(guestSession, &local, &guestGgpo.local) != GGPO_OK) { failure = "ggpo_guest_player"; break; }
        GGPOPlayer remote{}; remote.size = sizeof(remote); remote.type = GGPO_PLAYERTYPE_REMOTE;
        strcpy_s(remote.u.remote.ip_address, "127.0.0.1"); remote.player_num = 2; remote.u.remote.port = hostProxy.BridgePort();
        GGPOPlayerHandle ignored{}; activeNativeGgpo = &hostGgpo;
        if (ggpo_add_player(hostSession, &remote, &ignored) != GGPO_OK) { failure = "ggpo_host_remote"; break; }
        hostGgpo.remote = ignored;
        remote.player_num = 1; remote.u.remote.port = guestProxy.BridgePort(); activeNativeGgpo = &guestGgpo;
        if (ggpo_add_player(guestSession, &remote, &ignored) != GGPO_OK) { failure = "ggpo_guest_remote"; break; }
        guestGgpo.remote = ignored;

        const auto runningDeadline = GetTickCount64() + static_cast<ULONGLONG>(options.timeoutMs);
        while ((!hostGgpo.running || !guestGgpo.running) && GetTickCount64() < runningDeadline) {
            activeNativeGgpo = &hostGgpo; RecordNativeGgpoError(hostGgpo, ggpo_idle(hostSession, 0));
            activeNativeGgpo = &guestGgpo; RecordNativeGgpoError(guestGgpo, ggpo_idle(guestSession, 0));
            run.Pump(); Sleep(1);
        }
        if (!hostGgpo.running || !guestGgpo.running) { failure = "ggpo_running_timeout"; break; }

        // No new GGPO polling during this barrier: let handshake datagrams
        // settle, then require an exact counter baseline before any input.
        if (!run.Wait([&] {
                return hostProxy.Settled() && guestProxy.Settled() &&
                    NativeRun::MatchesProxy(run.hostStats, hostProxy) &&
                    NativeRun::MatchesProxy(run.guestStats, guestProxy);
            }, 5000)) { failure = "prework_statistics_barrier"; break; }
        if (!hostProxy.ArmWork() || !guestProxy.ArmWork()) { failure = "proxy_workload_arm"; break; }

        // GGPO's network_stats ping is a cached estimate. Clear handshake
        // samples before the measured interval and keep the helper resource
        // bracket aligned with the same workload.
        hostGgpo.pingUs.clear(); guestGgpo.pingUs.clear();
        run.BeginWork();
        before = SampleHelpers(run.hostProcess, run.guestProcess);
        sampledPeak = before.host.workingSet + before.guest.workingSet;
        resourceSamples = 1;
        workloadStarted = std::chrono::steady_clock::now();
        auto nextResourceSample = workloadStarted;
        const auto benchmarkDeadline = GetTickCount64() + static_cast<ULONGLONG>(options.timeoutMs);
        bool hostInputSubmitted = false, guestInputSubmitted = false;
        auto nextTick = std::chrono::steady_clock::now();
        while ((hostGgpo.acceptedFrames < static_cast<std::uint64_t>(options.frames) ||
            guestGgpo.acceptedFrames < static_cast<std::uint64_t>(options.frames)) &&
            GetTickCount64() < benchmarkDeadline) {
            run.Pump();
            activeNativeGgpo = &hostGgpo; RecordNativeGgpoError(hostGgpo, ggpo_idle(hostSession, 0));
            activeNativeGgpo = &guestGgpo; RecordNativeGgpoError(guestGgpo, ggpo_idle(guestSession, 0));
            SampleNativePing(hostGgpo); SampleNativePing(guestGgpo);
            const auto addInput = [&](NativeGgpoContext& context, bool& submitted, int player) {
                if (submitted || context.acceptedFrames >= static_cast<std::uint64_t>(options.frames)) return;
                unsigned char input = static_cast<unsigned char>((options.seed +
                    static_cast<std::uint32_t>(context.currentFrame) * (player == 1 ? 17u : 31u) +
                    static_cast<std::uint32_t>(player * 13)) & 255u);
                activeNativeGgpo = &context;
                const auto code = ggpo_add_local_input(context.session, context.local, &input, 1);
                RecordNativeGgpoError(context, code);
                if (code == GGPO_OK) submitted = true;
            };
            addInput(hostGgpo, hostInputSubmitted, 1);
            addInput(guestGgpo, guestInputSubmitted, 2);
            const auto advance = [&](NativeGgpoContext& context, bool& submitted) {
                if (!submitted || context.acceptedFrames >= static_cast<std::uint64_t>(options.frames)) return;
                unsigned char inputs[2]{}; int disconnected = 0;
                activeNativeGgpo = &context;
                const auto sync = ggpo_synchronize_input(context.session, inputs, sizeof(inputs), &disconnected);
                RecordNativeGgpoError(context, sync);
                if (disconnected) { ++context.disconnectedEvents; ++context.fatalErrors; return; }
                if (sync != GGPO_OK) return; // retain this input for the next paced tick
                // The benchmark owns the normal forward simulation step. The
                // GGPO advance callback is reserved for rollback replay, so
                // fold the confirmed synchronized inputs into the deterministic
                // state before advancing the native frame horizon.
                MixNativeState(context, context.currentFrame, inputs, sizeof(inputs));
                const auto code = ggpo_advance_frame(context.session);
                RecordNativeGgpoError(context, code);
                if (code == GGPO_OK) {
                    ++context.acceptedFrames; ++context.currentFrame; submitted = false;
                    context.currentStateFrame = context.currentFrame;
                }
            };
            advance(hostGgpo, hostInputSubmitted);
            advance(guestGgpo, guestInputSubmitted);
            if (std::chrono::steady_clock::now() >= nextResourceSample) {
                const auto sample = SampleHelpers(run.hostProcess, run.guestProcess);
                if (!sample.host.valid || !sample.guest.valid) { failure = "resource_sample"; break; }
                sampledPeak = std::max(sampledPeak, sample.host.workingSet + sample.guest.workingSet);
                ++resourceSamples;
                nextResourceSample = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
            if (!run.error.empty() || hostGgpo.fatalErrors || guestGgpo.fatalErrors || run.disconnectedDuringWork) break;
            nextTick += std::chrono::microseconds(16667);
            const auto now = std::chrono::steady_clock::now();
            if (now < nextTick) std::this_thread::sleep_for(nextTick - now);
            else nextTick = now;
        }
        completed = hostGgpo.acceptedFrames == static_cast<std::uint64_t>(options.frames) &&
            guestGgpo.acceptedFrames == static_cast<std::uint64_t>(options.frames);
        if (!completed) failure = "ggpo_frame_timeout";

        // Accepted forward frames may still contain prediction. idle receives
        // final inputs, performs rollback, and advances only confirmation.
        const auto confirmedDeadline = GetTickCount64() + static_cast<ULONGLONG>(options.timeoutMs);
        while (completed && GetTickCount64() < confirmedDeadline) {
            activeNativeGgpo = &hostGgpo; RecordNativeGgpoError(hostGgpo, ggpo_idle(hostSession, 0));
            activeNativeGgpo = &guestGgpo; RecordNativeGgpoError(guestGgpo, ggpo_idle(guestSession, 0));
            RecordNativeGgpoError(hostGgpo, ggpo_get_last_confirmed_frame(hostSession, &hostConfirmed));
            RecordNativeGgpoError(guestGgpo, ggpo_get_last_confirmed_frame(guestSession, &guestConfirmed));
            run.Pump();
            confirmed = hostConfirmed >= options.frames - 1 && guestConfirmed >= options.frames - 1 &&
                hostGgpo.currentFrame == options.frames && guestGgpo.currentFrame == options.frames &&
                hostGgpo.simulationFrame == options.frames && guestGgpo.simulationFrame == options.frames &&
                hostGgpo.currentStateFrame == options.frames && guestGgpo.currentStateFrame == options.frames;
            if (confirmed || hostGgpo.fatalErrors || guestGgpo.fatalErrors || !run.error.empty()) break;
            Sleep(2);
        }
        if (!confirmed && failure.empty()) failure = "ggpo_confirmation_timeout";
        // Stop polling GGPO, but keep its sockets and both helpers alive while
        // pending packets drain. Drains are requested concurrently.
        hostProxy.RequestDrain(); guestProxy.RequestDrain();
        hostProxy.AwaitDrain(); guestProxy.AwaitDrain();
        proxyQuiesced = hostProxy.Quiesced() && guestProxy.Quiesced();
        after = SampleHelpers(run.hostProcess, run.guestProcess);
        sampledPeak = std::max(sampledPeak, after.host.workingSet + after.guest.workingSet);
        ++resourceSamples;
        workloadUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - workloadStarted).count();
        run.EndWork();
        statsFresh = run.Wait([&] {
            return run.hostStatsEvents > run.hostStatsEventsAtEnd && run.guestStatsEvents > run.guestStatsEventsAtEnd &&
                NativeRun::MatchesProxy(run.hostStats, hostProxy) && NativeRun::MatchesProxy(run.guestStats, guestProxy);
        }, 5000);
        statsMatched = statsFresh && NativeRun::MatchesProxy(run.hostStats, hostProxy) &&
            NativeRun::MatchesProxy(run.guestStats, guestProxy);
        run.statsFrozen = statsMatched;
    } while (false);

    // Failure cleanup observes the same socket ownership ordering.
    hostProxy.RequestDrain(); guestProxy.RequestDrain();
    hostProxy.AwaitDrain(); guestProxy.AwaitDrain();
    proxyQuiesced = hostProxy.Quiesced() && guestProxy.Quiesced();
    run.endingRequested = true;
    if (hostSession) { activeNativeGgpo = &hostGgpo; if (ggpo_close_session(hostSession) != GGPO_OK) { ggpoCloseOk = false; if (failure.empty()) failure = "ggpo_host_close"; } hostSession = nullptr; }
    if (guestSession) { activeNativeGgpo = &guestGgpo; if (ggpo_close_session(guestSession) != GGPO_OK) { ggpoCloseOk = false; if (failure.empty()) failure = "ggpo_guest_close"; } guestSession = nullptr; }
    activeNativeGgpo = nullptr;
    bool teardownOk = true;
    bool endMatchSent = false;
    bool endMatchClosed = false;
    if (run.hostVirtual || run.guestVirtual) {
        const bool hostEnd = run.Send(run.host, json{{"type", "end_match"}, {"epoch", run.epoch}, {"generation", 1}});
        const bool guestEnd = run.Send(run.guest, json{{"type", "end_match"}, {"epoch", run.epoch}, {"generation", 1}});
        endMatchSent = hostEnd && guestEnd;
        teardownOk = endMatchSent;
        endMatchClosed = endMatchSent && run.Wait([&] { return run.hostVirtual == 0 && run.guestVirtual == 0; }, 5000);
        teardownOk = teardownOk && endMatchClosed;
    } else endMatchClosed = true;
    hostProxy.Stop(); guestProxy.Stop();
    teardownOk = teardownOk && proxyQuiesced;
    // The benchmark owns these isolated helpers. Shutdown directly after all
    // mappings close; room host transfer is tested by the recovery fixtures.
    const bool shutdownSent = run.Send(run.host, json{{"type", "shutdown"}}) &&
        run.Send(run.guest, json{{"type", "shutdown"}});
    const bool shutdownExited = shutdownSent && run.Wait([&] {
        return !run.hostProcess.IsRunning() && !run.guestProcess.IsRunning();
    }, 5000);
    teardownOk = teardownOk && shutdownExited;

    const auto fatalErrors = hostGgpo.fatalErrors + guestGgpo.fatalErrors;
    const auto resources = ResourceJson(before, after, sampledPeak);
    const bool resourceMeasured = !resources.is_null() && resources.value("measured", false);
    const bool wireMeasured = proxyQuiesced && hostProxy.SendErrors() == 0 && guestProxy.SendErrors() == 0 &&
        hostProxy.UnreadDropped() == 0 && guestProxy.UnreadDropped() == 0 &&
        hostProxy.Unsent() == 0 && guestProxy.Unsent() == 0 &&
        (hostProxy.Received() + guestProxy.Received()) != 0;
    const auto usableRoute = [](const std::string& route) {
        return !route.empty() && route != "unavailable" && route != "unknown";
    };
    const auto routeSamplesStable = [](const std::vector<std::string>& routes) {
        return routes.size() >= 2 && std::all_of(routes.begin(), routes.end(), [&](const std::string& route) {
            return route == routes.front();
        });
    };
    const bool routeStable = routeSamplesStable(run.hostGameRoutes) && routeSamplesStable(run.guestGameRoutes);
    const bool routeComparable = routeStable && RouteClass(run.hostGameRoute) == RouteClass(run.guestGameRoute);
    const bool routeMeasured = usableRoute(run.hostGameRoute) && usableRoute(run.guestGameRoute) && routeComparable;
    // Independent oracle: the wire carries one deterministic byte per fighter
    // at frame delay zero. Peer equality alone could hide matching fixture bugs.
    std::uint64_t expectedDigest = 1469598103934665603ULL;
    for (int frame = 0; frame < options.frames; ++frame) {
        expectedDigest ^= static_cast<std::uint64_t>(frame) + 0x9e3779b97f4a7c15ULL +
            (expectedDigest << 6) + (expectedDigest >> 2);
        for (unsigned player = 0; player < 2; ++player) {
            const auto input = (options.seed + static_cast<std::uint32_t>(frame) * (player ? 31u : 17u) +
                (player ? 26u : 13u)) & 255u;
            expectedDigest = (expectedDigest ^ (input + static_cast<std::uint64_t>(player) * 0x100000001b3ULL)) * 1099511628211ULL;
        }
    }
    const bool stateMeasured = confirmed && hostGgpo.hasChecksum && guestGgpo.hasChecksum &&
        hostGgpo.initialStateDigest == guestGgpo.initialStateDigest &&
        hostGgpo.finalStateDigest == expectedDigest && guestGgpo.finalStateDigest == expectedDigest;
    const bool ggpoArtifactMeasured = loadedGgpo.value("loaded", false) &&
        loadedGgpo.value("sha256", json(nullptr)).is_string();
    const bool fullMeasured = completed && failure.empty() && run.error.empty() && fatalErrors == 0 && hostGgpo.disconnectedEvents == 0 &&
        guestGgpo.disconnectedEvents == 0 && run.errors.empty() && !run.disconnectedDuringWork &&
        statsFresh && statsMatched && resourceMeasured && wireMeasured && routeMeasured && stateMeasured &&
        ggpoArtifactMeasured && ggpoCloseOk && teardownOk;
    result["measured"] = fullMeasured;
    result["phase_status"] = fullMeasured ? "complete" : (completed ? "completed_with_unmeasured_fields" : "incomplete");
    result["frames_requested"] = options.frames;
    result["frames_advanced"] = hostGgpo.acceptedFrames + guestGgpo.acceptedFrames;
    result["accepted_frame_counts"] = json{{"host", hostGgpo.acceptedFrames}, {"guest", guestGgpo.acceptedFrames},
        {"target_hz", 60}, {"pacing_interval_us", 16667}};
    result["confirmation"] = json{{"measured", confirmed}, {"target_input_frame", options.frames - 1},
        {"host_input_frame", hostConfirmed}, {"guest_input_frame", guestConfirmed},
        {"host_state_frame", hostGgpo.simulationFrame}, {"guest_state_frame", guestGgpo.simulationFrame}};
    result["prediction_stalls"] = hostGgpo.predictionStalls + guestGgpo.predictionStalls;
    result["not_synchronized"] = hostGgpo.notSynchronized + guestGgpo.notSynchronized;
    result["input_dropped"] = hostGgpo.inputDropped + guestGgpo.inputDropped;
    result["replay_loads"] = hostGgpo.loads + guestGgpo.loads;
    result["max_replay_depth"] = std::max(hostGgpo.maxReplayDepth, guestGgpo.maxReplayDepth);
    result["save_callbacks"] = hostGgpo.saves + guestGgpo.saves;
    result["fatal_callback_errors"] = fatalErrors;
    result["disconnected_events"] = hostGgpo.disconnectedEvents + guestGgpo.disconnectedEvents;
    result["sync_failures"] = hostGgpo.syncFailures + guestGgpo.syncFailures;
    result["ggpo_dll"] = loadedGgpo;
    result["callback_advances"] = hostGgpo.advances + guestGgpo.advances;
    result["ggpo_rtt_estimate"] = LatencyJson([&] {
        std::vector<std::uint64_t> samples = hostGgpo.pingUs;
        samples.insert(samples.end(), guestGgpo.pingUs.begin(), guestGgpo.pingUs.end());
        return samples;
    }());
    result["ggpo_rtt_estimate_source"] = "cached GGPO network_stats ping estimate sampled only during the workload; it is not helper forwarding latency";
    result["ggpo_ping_samples"] = hostGgpo.pingUs.size() + guestGgpo.pingUs.size();
    result["state_checksums"] = json{
        {"host_initial", hostGgpo.hasChecksum ? json(hostGgpo.initialChecksum) : json(nullptr)},
        {"guest_initial", guestGgpo.hasChecksum ? json(guestGgpo.initialChecksum) : json(nullptr)},
        {"host_final", hostGgpo.hasChecksum ? json(hostGgpo.finalChecksum) : json(nullptr)},
        {"guest_final", guestGgpo.hasChecksum ? json(guestGgpo.finalChecksum) : json(nullptr)},
        {"initial_equal", hostGgpo.hasChecksum && guestGgpo.hasChecksum &&
            hostGgpo.initialChecksum == guestGgpo.initialChecksum},
        {"final_equal", hostGgpo.hasChecksum && guestGgpo.hasChecksum &&
            hostGgpo.finalChecksum == guestGgpo.finalChecksum},
        {"available", hostGgpo.hasChecksum && guestGgpo.hasChecksum},
        {"host_digest", hostGgpo.finalStateDigest}, {"guest_digest", guestGgpo.finalStateDigest},
        {"expected_digest", expectedDigest}, {"matches_oracle", stateMeasured},
        {"basis", "deterministic input-driven benchmark state digest through GGPO save/load callbacks; not an SF4 gameplay state checksum"},
        {"state_measured", stateMeasured}};
    result["helper_statistics_matched"] = statsMatched;
    result["helper_statistics_fresh"] = statsFresh;
    result["helper_statistics"] = json{
        {"host_before_events", run.hostStatsEventsBefore}, {"host_after_events", run.hostStatsEvents},
        {"guest_before_events", run.guestStatsEventsBefore}, {"guest_after_events", run.guestStatsEvents},
        {"host_events_at_work_end", run.hostStatsEventsAtEnd}, {"guest_events_at_work_end", run.guestStatsEventsAtEnd},
        {"host_counter_deltas", NativeRun::CounterDeltas(run.hostStatsBefore, run.hostStats)},
        {"guest_counter_deltas", NativeRun::CounterDeltas(run.guestStatsBefore, run.guestStats)},
        {"fresh_postwork_samples", statsFresh}, {"host_before", run.hostStatsBefore}, {"host_after", run.hostStats},
        {"guest_before", run.guestStatsBefore}, {"guest_after", run.guestStats}};
    result["wire_proxy"] = json{{"measured", wireMeasured},
        {"host_received", hostProxy.Received()}, {"guest_received", guestProxy.Received()},
        {"host_forwarded", hostProxy.Forwarded()}, {"guest_forwarded", guestProxy.Forwarded()},
        {"host_dropped", hostProxy.Dropped()}, {"guest_dropped", guestProxy.Dropped()},
        {"host_unsent", hostProxy.Unsent()}, {"guest_unsent", guestProxy.Unsent()},
        {"host_send_errors", hostProxy.SendErrors()}, {"guest_send_errors", guestProxy.SendErrors()},
        {"host_unread_dropped", hostProxy.UnreadDropped()}, {"guest_unread_dropped", guestProxy.UnreadDropped()},
        {"delay_ms", options.delayMs}, {"drop_every", options.dropEvery},
        {"host_schedule_digest", hostProxy.ScheduleDigest()}, {"guest_schedule_digest", guestProxy.ScheduleDigest()},
        {"host_direction_digests", json{{"game_to_helper", hostProxy.DirectionScheduleDigest(0)}, {"helper_to_game", hostProxy.DirectionScheduleDigest(1)}}},
        {"guest_direction_digests", json{{"game_to_helper", guestProxy.DirectionScheduleDigest(0)}, {"helper_to_game", guestProxy.DirectionScheduleDigest(1)}}},
        {"host_direction_received", json{{"game_to_helper", hostProxy.DirectionReceived(0)}, {"helper_to_game", hostProxy.DirectionReceived(1)}}},
        {"guest_direction_received", json{{"game_to_helper", guestProxy.DirectionReceived(0)}, {"helper_to_game", guestProxy.DirectionReceived(1)}}},
        {"host_direction_forwarded", json{{"game_to_helper", hostProxy.DirectionForwarded(0)}, {"helper_to_game", hostProxy.DirectionForwarded(1)}}},
        {"guest_direction_forwarded", json{{"game_to_helper", guestProxy.DirectionForwarded(0)}, {"helper_to_game", guestProxy.DirectionForwarded(1)}}},
        {"host_direction_received_bytes", json{{"game_to_helper", hostProxy.DirectionReceivedBytes(0)}, {"helper_to_game", hostProxy.DirectionReceivedBytes(1)}}},
        {"guest_direction_received_bytes", json{{"game_to_helper", guestProxy.DirectionReceivedBytes(0)}, {"helper_to_game", guestProxy.DirectionReceivedBytes(1)}}},
        {"host_direction_forwarded_bytes", json{{"game_to_helper", hostProxy.DirectionForwardedBytes(0)}, {"helper_to_game", hostProxy.DirectionForwardedBytes(1)}}},
        {"guest_direction_forwarded_bytes", json{{"game_to_helper", guestProxy.DirectionForwardedBytes(0)}, {"helper_to_game", guestProxy.DirectionForwardedBytes(1)}}},
        {"handshake_unimpaired", true}, {"schedule_armed_at_prework_barrier", true}, {"quiesced", proxyQuiesced}};
    result["resources"] = resources;
    result["resource_interval"] = json{{"wall_us", workloadUs}, {"working_set_samples", resourceSamples},
        {"scope", "both helpers during forward stepping, final confirmation, and bounded packet drain; excludes startup, post-drain statistics wait, and teardown"}};
    const auto hostDeltas = NativeRun::CounterDeltas(run.hostStatsBefore, run.hostStats);
    const auto guestDeltas = NativeRun::CounterDeltas(run.guestStatsBefore, run.guestStats);
    const auto helperOperations = NativeRun::Counter(hostDeltas, "sent_packets") + NativeRun::Counter(hostDeltas, "received_packets") +
        NativeRun::Counter(guestDeltas, "sent_packets") + NativeRun::Counter(guestDeltas, "received_packets");
    result["forwarding_cost"] = json{{"measured", fullMeasured && helperOperations != 0},
        {"helper_forward_operations", helperOperations},
        {"helper_cpu_us_per_operation", fullMeasured && helperOperations ?
            json(resources.at("helper_cpu_ms").get<double>() * 1000.0 / static_cast<double>(helperOperations)) : json(nullptr)},
        {"scope", "amortized total helper CPU per successful QUIC enqueue or local UDP delivery during the measured interval, including room coordination and drain idle time; not isolated bridge latency"}};
    result["transport_residual_packets"] = json{
        {"host_to_guest", static_cast<std::int64_t>(NativeRun::Counter(run.hostStats, "sent_packets")) -
            static_cast<std::int64_t>(NativeRun::Counter(run.guestStats, "received_packets"))},
        {"guest_to_host", static_cast<std::int64_t>(NativeRun::Counter(run.guestStats, "sent_packets")) -
            static_cast<std::int64_t>(NativeRun::Counter(run.hostStats, "received_packets"))},
        {"scope", "lifetime mapping QUIC enqueue minus remote local delivery; separate from planned proxy drops"}};
    if (!run.hostGameRoute.empty() || !run.guestGameRoute.empty())
        result["selected_route"] = json{{"host", run.hostGameRoute}, {"guest", run.guestGameRoute},
            {"host_class", RouteClass(run.hostGameRoute)}, {"guest_class", RouteClass(run.guestGameRoute)}};
    result["route_status"] = routeMeasured ? json("periodic_selected_path_samples_unchanged") : json("unavailable_or_changed");
    result["route_observations"] = json{{"host", run.hostGameRoutes}, {"guest", run.guestGameRoutes},
        {"scope", "selected path sampled with periodic bridge statistics before, during, and after workload; changes between samples are not observable"}};
    result["route_measured"] = routeMeasured;
    result["route_stable"] = routeStable;
    result["route_comparable"] = routeComparable;
    result["teardown"] = json{{"end_match_sent", endMatchSent}, {"end_match_closed", endMatchClosed},
        {"ggpo_close_ok", ggpoCloseOk}, {"proxy_quiesced", proxyQuiesced}, {"shutdown_exited", shutdownExited},
        {"measured", teardownOk && ggpoCloseOk}};
    result["runtime_errors"] = run.errors;
    result["helper_game_statistics"] = json{{"host", run.hostStats}, {"guest", run.guestStats}};
    result["wall_us"] = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (!failure.empty()) result["error"] = failure;
    else if (!run.error.empty()) result["error"] = run.error;
    result["native_gameplay"] = false;
    result["notes"] = "Real GGPO datagrams crossed the selected helper game_ready route through a deterministic per-direction loopback proxy; native callbacks use an input-driven save/load state digest; GGPO RTT is a cached estimate, and no SF4 game process was launched.";
    return result;
}

bool ParseBoolSwitch(const std::wstring& value) { return value == L"1" || value == L"true"; }
Options ParseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto value = [&](const wchar_t* name) -> std::optional<std::wstring> {
            const std::wstring prefix = std::wstring(name) + L"=";
            return arg.rfind(prefix, 0) == 0 ? std::optional<std::wstring>(arg.substr(prefix.size())) : std::nullopt;
        };
        if (auto v = value(L"--helper")) options.helper = *v;
        else if (auto v = value(L"--seed")) options.seed = static_cast<std::uint32_t>(std::stoul(*v));
        else if (auto v = value(L"--frames")) options.frames = std::max(1, std::stoi(*v));
        else if (auto v = value(L"--drop-every")) options.dropEvery = std::stoi(*v);
        else if (auto v = value(L"--delay-ms")) options.delayMs = std::max(0, std::stoi(*v));
        else if (auto v = value(L"--timeout-ms")) options.timeoutMs = std::max(1000, std::stoi(*v));
        else if (auto v = value(L"--output")) options.output = *v;
        else if (arg == L"--skip-native") options.skipNative = true;
        else if (arg == L"--skip-ggpo") options.skipGgpo = true;
        else if (arg == L"--relay-only") options.relayOnly = true;
        else if (arg == L"--rift") options.rift = true;
        else if (arg == L"--coarse") options.continuous = false;
        else if (auto v = value(L"--jitter-ms")) options.jitterMs = std::max(0, std::stoi(*v));
        else if (auto v = value(L"--burst-ms")) options.burstMs = std::max(0, std::stoi(*v));
        else if (auto v = value(L"--burst-every-ms")) options.burstEveryMs = std::max(0, std::stoi(*v));
        else if (auto v = value(L"--input-delay")) options.inputDelay = std::clamp(std::stoi(*v), 0, 10);
        else if (auto v = value(L"--fast-hz")) options.fastHz = std::clamp(std::stod(*v), 30.0, 120.0);
    }
    return options;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto options = ParseOptions(argc, argv);
    if (options.rift) {
        WSADATA riftWinsock{};
        if (WSAStartup(MAKEWORD(2, 2), &riftWinsock) != 0) return 3;
        const auto rift = RunRift(options).dump(2);
        WSACleanup();
        if (!options.output.empty()) std::ofstream(NarrowPath(options.output), std::ios::binary) << rift << "\n";
        std::cout << rift << "\n";
        return 0;
    }
    if (options.helper.empty() && !options.skipNative) {
        std::wcerr << L"usage: recovery_benchmark --helper=<sf4-net.exe> [--seed=N --frames=N --drop-every=N --delay-ms=N]\n";
        return 2;
    }
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 3;
    json output{{"schema", "sf4.recovery-benchmark.v1"}, {"seed", options.seed}, {"frames", options.frames},
        {"schedule", json{{"application_delay_ms", options.delayMs}, {"application_drop_every", options.dropEvery},
            {"native_packet_proxy", true}, {"synthetic_ggpo_packet_proxy", true}, {"relay_only", options.relayOnly},
            {"pacing_interval_us", 16667}}},
        {"native", options.skipNative ? json(nullptr) : RunNativeGgpo(options)},
        {"ggpo", options.skipGgpo ? json(nullptr) : RunSyntheticGgpo(options)}};
    WSACleanup();
    const auto serialized = output.dump(2);
    if (!options.output.empty()) {
        const auto outputPath = NarrowPath(options.output);
        if (outputPath.empty()) {
            std::wcerr << L"output path conversion failed\n";
            return 4;
        }
        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open()) {
            std::wcerr << L"output file could not be opened\n";
            return 5;
        }
        file << serialized << "\n";
        if (!file.good()) {
            std::wcerr << L"output file write failed\n";
            return 6;
        }
    }
    std::cout << serialized << "\n";
    return 0;
}

