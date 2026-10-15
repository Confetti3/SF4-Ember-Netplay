#pragma once
#include "../launcher/update/github_release_client.hxx"
#include "../common/NetworkNat.hxx"
#include "../common/NetworkRoute.hxx"
#include "ReportWorkflow.hxx"
#include <memory>
#include <chrono>
#include <condition_variable>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <atomic>

namespace sf4e { namespace platform {
// SwitchUpdateChannel saves the channel ApplicationServices::SwitchUpdateChannel
// names, then checks for an update on it.
// ShowReplayFile opens, in Explorer, the folder that holds the archived
// replay the request names; only a folder of the archive is opened.
enum class ServiceAction { None, CheckUpdates, SwitchUpdateChannel, ExportDiagnostics, OpenUpdater, InstallUpdate, OpenRecovery, OpenCommunity, OpenReplayFolder, ShowReplayFile,
    PrepareProblemReport, PrepareCrashReport, SendReport, CancelReport, ReportCrash };
// Community Discord server, shown in Help & About and opened as https://<invite>.
constexpr const char* CommunityInvite = "discord.gg/uPNqF5A5uq";
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
    FreeState,
    EffectRestore,
    VfxRestore,
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
    int probeState=0;
    RouteKind probeRoute=RouteKind::Unknown;
    // The relay region of the measured route when it is relayed, and the local
    // network summary. Region codes and classes only, never an address.
    std::string probeRelay;
    NetworkSummary netReport;
    // The helper's fixed UDP port, 0 when the OS chose it, or empty when no
    // helper has reported one.
    std::optional<std::uint16_t> udpPort;
    unsigned probeFailure=0;
    unsigned sent=0, expected=0;
    bool benchmark=false;
    unsigned replies=0, missed=0, directLinks=0, relayedLinks=0;
    std::uint64_t p50Us=0,p95Us=0,p99Us=0,jitterUs=0,routeChanges=0,localDrops=0,sendPressure=0;
};
struct ServiceSnapshot {
    bool pending = false, closeGame = false, installed = false;
    // The finished action did what it was asked, so `message` is good news
    // (up to date, update found, saved). False while pending, on failure and
    // on cancellation.
    bool succeeded = false;
    // The action `message` describes, so the interface can show it on the
    // row that requested it rather than on whichever row happens to bind it.
    ServiceAction lastAction = ServiceAction::None;
    std::string message;
    // The step an update being fetched is at, and how far: bytes while
    // downloading and verifying, files after. A total of 0 is not known.
    launcher::UpdateStage updateStage = launcher::UpdateStage::Downloading;
    std::uint64_t stageDone = 0, stageTotal = 0;
    launcher::UpdateCheckResult update;
    // The version on this PC and the channel its checks use, read once at
    // start; the worker changes the channel, so no one else reads the file.
    std::string installedVersion;
    launcher::UpdateChannel channel = launcher::UpdateChannel::Stable;
    std::vector<std::string> connectionHistory;
    reports::WorkflowState reporting;
    // The reports this PC sent, oldest first: read at start and after each
    // report operation. Unreadable, it stops every automatic send.
    std::vector<reports::Record> sentReports;
    bool sentReadable = true;
};
// The home relay and network class, once per export: they rarely change, so
// the connection history lines leave them out.
std::string DescribeNetwork(const DiagnosticsView& view);
std::string DescribeDiagnostics(const DiagnosticsView& view);
// A single bounded worker owns filesystem, HTTP and process operations.
// Report previews contain only the fixed metadata, redacted log tails and
// the optional small dump; they are held unchanged until consent.
class ApplicationServices {
public:
    explicit ApplicationServices(std::wstring diagnosticsDirectory = {});
    ~ApplicationServices();
    // target: the file a ShowReplayFile request names; nothing else reads it.
    bool Request(ServiceAction action, const DiagnosticsView& diagnostics = {}, std::wstring target = {});
    // Saves `channel` as the update channel, then checks for an update on it.
    bool SwitchUpdateChannel(launcher::UpdateChannel channel);
    // The crash's report in a preview, sent only from it.
    bool PrepareCrashReport(const reports::CrashContext& crash);
    // The crash's report sent without a press, when the worker finds it
    // allowed (ReportWorkflow.hxx: AutomaticCrash); otherwise it is offered.
    bool ReportCrash(const reports::CrashContext& crash, reports::AutomaticCrash::Consent consent);
    bool SendReport(const reports::Submission& submission);
    ServiceSnapshot Snapshot() const;
    void Cancel() { cancelled_ = true; }
    void Observe(const DiagnosticsView& diagnostics);
private:
    // Starts `action` on the worker; the caller holds mutex_.
    bool Start(ServiceAction action, const DiagnosticsView& diagnostics, launcher::UpdateChannel channel, std::wstring target = {});
    // Starts a report operation through Start; the caller holds mutex_.
    bool QueueReport(ServiceAction action, reports::Operation operation);
    void Run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    ServiceAction request_ = ServiceAction::None;
    launcher::UpdateChannel requestChannel_ = launcher::UpdateChannel::Stable;
    DiagnosticsView diagnostics_;
    std::wstring target_;
    std::optional<reports::Operation> reportOperation_;
    ServiceSnapshot state_;
    std::wstring diagnosticsDirectory_;
    std::shared_ptr<reports::ReportHistory> history_;
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
};
} }
