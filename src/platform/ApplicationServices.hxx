#pragma once
#include "../launcher/update/github_release_client.hxx"
#include <condition_variable>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

namespace sf4e { namespace platform {
enum class ServiceAction { None, CheckUpdates, ExportDiagnostics, OpenUpdater, InstallUpdate, OpenRecovery };
enum class DiagnosticTiming : std::size_t {
    CompleteOuterCall,
    OuterTick,
    RoomRuntime,
    SessionClientStep,
    SessionServerStep,
    GgpoIdle,
    RollbackCallback,
    SaveState,
    LoadState,
    PacingWait,
    DiagnosticEnqueue,
    TraceEnqueue,
    Count
};
constexpr std::size_t DiagnosticTimingCount = static_cast<std::size_t>(DiagnosticTiming::Count);
struct DiagnosticsView {
    struct Timing { std::uint64_t count=0, over25Ms=0; double meanMs=0, maxMs=0; };
    bool performanceEnabled=false;
    // Snapshot of game-thread counters; the export worker never reads live state.
    std::array<Timing, DiagnosticTimingCount> timings{};
    Timing& TimingAt(DiagnosticTiming timing) { return timings[static_cast<std::size_t>(timing)]; }
    const Timing& TimingAt(DiagnosticTiming timing) const { return timings[static_cast<std::size_t>(timing)]; }
    std::uint64_t rollbackCallbacks=0, predictionStalls=0, predictionSkippedFrames=0;
    std::uint64_t traceDropped=0, logDropped=0;
    double traceLastWriteMs=0;
    bool recoveryCheckpointBuildsAvailable=false;
    std::uint64_t recoveryCheckpointBuilds=0;
    int selectedDelay=-1;
    int room = 0, match = 0, control = 0, gameplay = 0, pingMs = -1;
    bool helperReady = false, verificationAvailable = false;
    // Typed allowlist: no endpoint addresses, identities, credentials or names.
    int probeState=0, probeRoute=0;
    unsigned probeFailure=0;
    unsigned sent=0, expected=0;
    bool benchmark=false;
    unsigned replies=0, missed=0, directLinks=0, relayedLinks=0;
    std::uint64_t p50Us=0,p95Us=0,p99Us=0,jitterUs=0,routeChanges=0,localDrops=0,sendPressure=0;
};
struct ServiceSnapshot {
    bool pending = false, closeGame = false, installed = false;
    std::string message;
    std::uint64_t downloadedBytes = 0, totalBytes = 0;
    launcher::UpdateCheckResult update;
    std::vector<std::string> connectionHistory;
};
std::string DescribeDiagnostics(const DiagnosticsView& view);
// A single bounded worker owns filesystem, HTTP and process operations. Views
// contain no invitations, names, capabilities, arbitrary logs or settings.
class ApplicationServices {
public:
    explicit ApplicationServices(std::wstring diagnosticsDirectory = {});
    ~ApplicationServices();
    bool Request(ServiceAction action, const DiagnosticsView& diagnostics = {});
    ServiceSnapshot Snapshot() const;
    void Cancel() { cancelled_ = true; }
    void Observe(const DiagnosticsView& diagnostics);
private:
    void Run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    ServiceAction request_ = ServiceAction::None;
    DiagnosticsView diagnostics_;
    ServiceSnapshot state_;
    std::wstring diagnosticsDirectory_;
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
};
} }
