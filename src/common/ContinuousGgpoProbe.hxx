#pragma once

// Bounded, offline diagnostic. These are two real GGPO P2P sessions whose
// endpoints are both this process. Only the first drives the supplied engine;
// the second deliberately delivers changing, otherwise unused input late.
// No room, game-global GGPO pointer, scores, or native engine API belongs here.
#include <winsock2.h>
#include <windows.h>
#include <ggponet.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace sf4e { namespace continuousprobe {

constexpr int RestartFrame = 16;
constexpr int FrameLimit = 600;
constexpr int ShadowLag = 4;
constexpr std::uint64_t SyncTimeoutMs = 10000;
constexpr std::uint64_t TotalTimeoutMs = 45000;
static_assert(sizeof(void*) == 4, "The native probe and its driver test use x86");
static_assert(ShadowLag + 1 < GGPO_MAX_PREDICTION_FRAMES, "Leave room for one outer update");

struct Engine {
    virtual ~Engine() = default;
    // A token returned even by a failed Save remains owned until Free.
    virtual bool Save(void*& token) = 0;
    virtual bool Load(void* token) = 0;
    virtual void Free(void* token) = 0;
    virtual bool Advance(int globalFrame, bool resimulation) = 0;
};

struct Stats {
    std::array<int, 2> frames{{0, 0}};
    std::array<int, 2> confirmed{{-1, -1}};
    std::array<unsigned, 2> runningEvents{{0, 0}};
    std::array<unsigned, 2> stalls{{0, 0}};
    unsigned opens = 0, closes = 0;
    unsigned saves = 0, frees = 0, loads = 0, replayFrames = 0;
    unsigned resetCrossingLoads = 0, shadowLoads = 0, shadowReplayFrames = 0;
    unsigned maxDepth = 0, deferredCloses = 0;
    std::uint64_t startedMs = 0, runningMs = 0, completedMs = 0;
};

class Driver {
public:
    explicit Driver(Engine& engine) : engine_(engine) {}
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    ~Driver() { Close(); }

    bool Start() {
        if (started_) return false;
        started_ = true;
        thread_ = GetCurrentThreadId();
        stats_.startedMs = GetTickCount64();
        if (Owner() && Owner() != this) {
            Fail("another-private-driver-active"); finished_ = true; return false;
        }
        Owner() = this;
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            Fail("winsock-start-failed"); Close(); return false;
        }
        winsock_ = true;
        PortReservation reservations[2];
        const auto rejectStart = [&]() {
            // Reserved sockets must close before our final WSACleanup, also
            // when startup fails before either GGPO socket was constructed.
            for (auto& reservation : reservations) reservation.Release();
            Close();
            return false;
        };
        if (!reservations[0].Bind() || !reservations[1].Bind() ||
            reservations[0].port == reservations[1].port) {
            Fail("loopback-port-reservation-failed"); return rejectStart();
        }
        const unsigned short ports[2]{reservations[0].port, reservations[1].port};
        const auto callbacks = Callbacks();
        for (int side = 0; side != 2; ++side) {
            Peer& peer = peers_[side];
            peer.side = side;
            // Keep the other reservation bound until its own GGPO socket opens.
            reservations[side].Release();
            {
                ApiScope context(*this, peer);
                auto copy = callbacks;
                if (ggpo_start_session(&peer.session, &copy, "sf4e-private-native-probe", 2, 1, ports[side]) != GGPO_OK) {
                    Fail("ggpo-start-failed");
                }
            }
            if (peer.session) ++stats_.opens;
            if (!failure_.empty() || !peer.session) return rejectStart();
            GGPOPlayer player{};
            player.size = sizeof(player);
            player.type = GGPO_PLAYERTYPE_LOCAL;
            player.player_num = side + 1;
            {
                ApiScope context(*this, peer);
                if (ggpo_add_player(peer.session, &player, &peer.local) != GGPO_OK ||
                    ggpo_set_frame_delay(peer.session, peer.local, 0) != GGPO_OK)
                    Fail("ggpo-local-player-failed");
            }
            if (!failure_.empty()) return rejectStart();
            player.type = GGPO_PLAYERTYPE_REMOTE;
            player.player_num = 2 - side;
            std::memcpy(player.u.remote.ip_address, "127.0.0.1", sizeof("127.0.0.1"));
            player.u.remote.port = ports[1 - side];
            GGPOPlayerHandle remote = GGPO_INVALID_HANDLE;
            {
                ApiScope context(*this, peer);
                if (ggpo_add_player(peer.session, &player, &remote) != GGPO_OK)
                    Fail("ggpo-remote-player-failed");
            }
            if (!failure_.empty()) return rejectStart();
        }
        return true;
    }

    // The caller owns pacing. One native forward update at most per Tick;
    // rollback callbacks may repeat previously simulated native updates.
    void Tick() {
        if (!Active() || finished_) return;
        if (GetCurrentThreadId() != thread_) { Fail("driver-thread-changed"); return; }
        if (apiDepth_ || engineDepth_ || callbackDepth_) { Fail("reentrant-driver-tick"); return; }
        if (!failure_.empty()) { Close(); return; }
        const auto now = GetTickCount64();
        if (now - stats_.startedMs >= TotalTimeoutMs) { Fail("private-probe-timeout"); Close(); return; }
        PollAll();
        if (!failure_.empty()) { Close(); return; }
        if (!peers_[0].running || !peers_[1].running) {
            if (now - stats_.startedMs >= SyncTimeoutMs) { Fail("private-sync-timeout"); Close(); }
            return;
        }
        if (!stats_.runningMs) stats_.runningMs = now;
        if (peers_[0].frame < FrameLimit) Forward(peers_[0]);
        if (!failure_.empty()) { Close(); return; }
        PollAll();
        if (!failure_.empty()) { Close(); return; }
        // At the cap release the remaining withheld inputs and confirm both
        // timelines before reporting success. A missing tail is not a pass.
        const int shadowTarget = peers_[0].frame == FrameLimit ? FrameLimit :
            (std::max)(0, peers_[0].frame - ShadowLag);
        while (peers_[1].frame < shadowTarget && failure_.empty()) {
            const int before = peers_[1].frame;
            Forward(peers_[1]);
            PollAll();
            if (peers_[1].frame == before) break;
        }
        if (!failure_.empty()) { Close(); return; }
        SampleStats();
        if (peers_[0].frame == FrameLimit && peers_[1].frame == FrameLimit &&
            stats_.confirmed[0] >= FrameLimit - 1 && stats_.confirmed[1] >= FrameLimit - 1) {
            if (!stats_.loads || !stats_.replayFrames || !stats_.resetCrossingLoads)
                Fail("required-native-rollback-not-observed");
            completed_ = failure_.empty();
            stats_.completedMs = GetTickCount64();
            Close();
        }
    }

    // A synchronous native close may happen inside Engine::Advance. Mark the
    // failure first, then retry Close after the outer Tick has unwound.
    bool Close() {
        if (callbackDepth_ || apiDepth_ || engineDepth_) { ++stats_.deferredCloses; return false; }
        if (started_ && GetCurrentThreadId() != thread_) { Fail("driver-close-thread-changed"); return false; }
        if (Active() && !completed_ && failure_.empty()) Fail("private-probe-closed-before-completion");
        for (auto& peer : peers_) {
            if (!peer.session) continue;
            GGPOSession* session = peer.session;
            {
                ApiScope context(*this, peer);
                if (ggpo_close_session(session) != GGPO_OK) Fail("ggpo-close-failed");
            }
            peer.session = nullptr;
            ++stats_.closes;
            // Supported GGPO frees all saved buffers in its destructor. A
            // survivor indicates a driver/library contract violation. Free it
            // once here so a failed probe cannot leak its engine ownership.
            for (auto& slot : peer.slots) {
                if (!slot.used) continue;
                Fail("ggpo-close-left-save-token");
                Release(slot);
            }
        }
        if (winsock_) { WSACleanup(); winsock_ = false; }
        if (Owner() == this) Owner() = nullptr;
        if (started_) finished_ = true;
        stats_.frames = {{peers_[0].frame, peers_[1].frame}};
        return true;
    }

    bool Active() const { return peers_[0].session || peers_[1].session; }
    bool Finished() const { return finished_; }
    bool Succeeded() const { return finished_ && completed_ && failure_.empty() && stats_.opens == 2 && stats_.closes == 2; }
    const std::string& Failure() const { return failure_; }
    void Fail(const char* reason) { if (failure_.empty()) failure_ = reason && *reason ? reason : "private-engine-failed"; }
    const Stats& GetStats() const { return stats_; }

private:
    // GGPO Sync::SavedState holds MAX_PREDICTION_FRAMES+2 buffers. A separate
    // wrapper for every possible head avoids allocation in save callbacks.
    static constexpr std::size_t SlotCount = GGPO_MAX_PREDICTION_FRAMES + 2;
    struct Slot { int frame = 0; void* token = nullptr; bool used = false; };
    struct Peer {
        GGPOSession* session = nullptr;
        GGPOPlayerHandle local = GGPO_INVALID_HANDLE;
        int side = 0, frame = 0;
        bool running = false, inputQueued = false;
        std::array<Slot, SlotCount> slots{};
    };
    struct PortReservation {
        SOCKET socketValue = INVALID_SOCKET;
        unsigned short port = 0;
        bool Bind() {
            socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socketValue == INVALID_SOCKET) return false;
            const BOOL exclusive = TRUE;
            if (setsockopt(socketValue, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) return false;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(socketValue, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return false;
            int length = sizeof(address);
            if (getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
            port = ntohs(address.sin_port);
            return port != 0;
        }
        void Release() { if (socketValue != INVALID_SOCKET) { closesocket(socketValue); socketValue = INVALID_SOCKET; } }
        ~PortReservation() { Release(); }
    };
    struct ApiScope {
        Driver& driver; Peer* previous;
        ApiScope(Driver& value, Peer& peer) : driver(value), previous(value.activePeer_) { ++driver.apiDepth_; driver.activePeer_ = &peer; }
        ~ApiScope() { driver.activePeer_ = previous; --driver.apiDepth_; }
    };
    struct EngineScope {
        Driver& driver;
        explicit EngineScope(Driver& value) : driver(value) { ++driver.engineDepth_; }
        ~EngineScope() { --driver.engineDepth_; }
    };
    struct CallbackScope {
        Driver& driver;
        explicit CallbackScope(Driver& value) : driver(value) { ++driver.callbackDepth_; }
        ~CallbackScope() { --driver.callbackDepth_; }
    };
    static Driver*& Owner() { static Driver* value = nullptr; return value; }
    static Driver& Current() { return *Owner(); }
    static bool __cdecl Begin(const char*) { return true; }
    static bool __cdecl Log(char*, unsigned char*, int) { return true; }
    static bool __cdecl Save(unsigned char** buffer, int* length, int* checksum, int frame) {
        auto& driver = Current(); CallbackScope scope(driver); auto& peer = *driver.activePeer_;
        if (peer.frame != frame) { driver.Fail("save-callback-frame-mismatch"); peer.frame = frame; }
        Slot* slot = nullptr;
        for (auto& candidate : peer.slots) if (!candidate.used) { slot = &candidate; break; }
        // Fixed-size GGPO ring and free-before-save guarantee a vacant slot.
        // Supply a valid wrapper even if engine saving fails.
        if (!slot) { driver.Fail("ggpo-save-ring-contract-violated"); return false; }
        slot->used = true; slot->frame = frame; slot->token = nullptr;
        if (peer.side == 0 && driver.failure_.empty()) {
            EngineScope engineScope(driver);
            try {
                if (!driver.engine_.Save(slot->token) || !slot->token) driver.Fail("native-save-failed");
            } catch (...) { driver.Fail("native-save-exception"); }
            ++driver.stats_.saves;
        }
        *buffer = reinterpret_cast<unsigned char*>(slot);
        *length = sizeof(Slot); *checksum = 0;
        return true;
    }
    Slot* Find(Peer& peer, const void* pointer) {
        for (auto& slot : peer.slots) if (&slot == pointer) return &slot;
        return nullptr;
    }
    static bool __cdecl Load(unsigned char* buffer, int length) {
        auto& driver = Current(); CallbackScope scope(driver); auto& peer = *driver.activePeer_;
        Slot* slot = driver.Find(peer, buffer);
        if (!slot || !slot->used || length != sizeof(Slot)) { driver.Fail("invalid-ggpo-load-token"); return false; }
        const int previous = peer.frame;
        // Set this even on a failed native restore. GGPO will still call back
        // for every replay frame and assert that its original head is reached.
        peer.frame = slot->frame;
        if (peer.side == 0) {
            ++driver.stats_.loads;
            driver.stats_.maxDepth = (std::max)(driver.stats_.maxDepth, static_cast<unsigned>((std::max)(0, previous - slot->frame)));
            if (previous > RestartFrame && slot->frame <= RestartFrame) ++driver.stats_.resetCrossingLoads;
            if (driver.failure_.empty()) {
                EngineScope engineScope(driver);
                try {
                    if (!slot->token || !driver.engine_.Load(slot->token)) driver.Fail("native-load-failed");
                } catch (...) { driver.Fail("native-load-exception"); }
            }
        } else ++driver.stats_.shadowLoads;
        return true;
    }
    void Release(Slot& slot) {
        if (slot.token) {
            EngineScope scope(*this);
            void* token = slot.token;
            slot.token = nullptr;
            try { engine_.Free(token); } catch (...) { Fail("native-free-exception"); }
            ++stats_.frees;
        }
        slot.used = false;
    }
    static void __cdecl Free(void* buffer) {
        if (!buffer) return; // GGPO's destructor also passes unused ring slots.
        auto& driver = Current(); CallbackScope scope(driver); auto& peer = *driver.activePeer_;
        Slot* slot = driver.Find(peer, buffer);
        if (!slot || !slot->used) { driver.Fail("invalid-ggpo-free-token"); return; }
        driver.Release(*slot);
    }
    static bool __cdecl Replay(int) {
        auto& driver = Current(); CallbackScope scope(driver); auto& peer = *driver.activePeer_;
        if (peer.side == 0) ++driver.stats_.replayFrames; else ++driver.stats_.shadowReplayFrames;
        driver.Simulate(peer, true);
        return true;
    }
    static bool __cdecl Event(GGPOEvent* event) {
        auto& driver = Current(); CallbackScope scope(driver); auto& peer = *driver.activePeer_;
        if (event->code == GGPO_EVENTCODE_RUNNING) { peer.running = true; ++driver.stats_.runningEvents[peer.side]; }
        if (event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) driver.Fail("private-peer-disconnected");
        return true;
    }
    static GGPOSessionCallbacks Callbacks() {
        GGPOSessionCallbacks cb{};
        cb.begin_game = Begin; cb.save_game_state = Save; cb.load_game_state = Load;
        cb.free_buffer = Free; cb.log_game_state = Log; cb.advance_frame = Replay; cb.on_event = Event;
        return cb;
    }
    void PollAll() {
        for (auto& peer : peers_) {
            if (!peer.session) continue;
            ApiScope scope(*this, peer);
            if (ggpo_idle(peer.session, 0) != GGPO_OK) Fail("private-ggpo-poll-failed");
            if (!failure_.empty()) break;
        }
        SampleStats();
    }
    void SampleStats() {
        for (int side = 0; side != 2; ++side) {
            auto& peer = peers_[side]; stats_.frames[side] = peer.frame;
            if (!peer.session) continue;
            ApiScope scope(*this, peer);
            if (ggpo_get_last_confirmed_frame(peer.session, &stats_.confirmed[side]) != GGPO_OK)
                Fail("private-confirmation-query-failed");
        }
    }
    bool Simulate(Peer& peer, bool replay) {
        ApiScope scope(*this, peer);
        unsigned char input[2]{}; int disconnected = 0;
        const auto sync = ggpo_synchronize_input(peer.session, input, sizeof(input), &disconnected);
        if (sync != GGPO_OK || disconnected) {
            Fail(disconnected ? "private-input-disconnected" : "private-input-sync-failed");
            // A replay must finish GGPO's frame accounting even after failure.
            if (!replay) return false;
        }
        if (peer.side == 0 && failure_.empty()) {
            EngineScope engineScope(*this);
            try {
                if (!engine_.Advance(peer.frame, replay)) Fail("native-advance-failed");
            } catch (...) { Fail("native-advance-exception"); }
        }
        ++peer.frame;
        if (ggpo_advance_frame(peer.session) != GGPO_OK) Fail("private-advance-failed");
        return true;
    }
    void Forward(Peer& peer) {
        ApiScope scope(*this, peer);
        if (!peer.inputQueued) {
            unsigned char input = peer.side ? static_cast<unsigned char>((peer.frame / ShadowLag) & 1) : 0;
            const auto result = ggpo_add_local_input(peer.session, peer.local, &input, sizeof(input));
            if (result == GGPO_ERRORCODE_PREDICTION_THRESHOLD || result == GGPO_ERRORCODE_NOT_SYNCHRONIZED) {
                ++stats_.stalls[peer.side]; return;
            }
            if (result != GGPO_OK) { Fail("private-add-input-failed"); return; }
            peer.inputQueued = true;
        }
        if (Simulate(peer, false)) peer.inputQueued = false;
    }

    Engine& engine_;
    Peer peers_[2];
    Peer* activePeer_ = nullptr;
    Stats stats_;
    std::string failure_;
    DWORD thread_ = 0;
    unsigned apiDepth_ = 0, engineDepth_ = 0, callbackDepth_ = 0;
    bool started_ = false, winsock_ = false, finished_ = false, completed_ = false;
};

} }
