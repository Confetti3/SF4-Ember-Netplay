#include "ApplicationServices.hxx"
#include "../common/install_paths.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../common/Localization.hxx"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace sf4e { namespace platform {
std::string DescribeDiagnostics(const DiagnosticsView& view) {
    const char* rooms[] = {"Idle", "Opening", "Joined", "Closing", "Lost"};
    const char* matches[] = {"None", "Preparing", "Playing", "Post-match", "Failed"};
    const char* health[] = {"Offline", "Connecting", "Healthy", "Lost"};
    const auto label = [](int value, const char* const* names, int count) { return value >= 0 && value < count ? names[value] : "Unavailable"; };
    const char* probes[] = {"Not checked","Checking","Complete","Invalidated","Unavailable","Timed out","Local overload"};
    const char* routes[] = {"Unknown","Direct","Relayed"};
    const char* probeFailures[] = {"Unspecified", "Room or request changed", "Helper busy", "Peer control unavailable",
        "Check already active", "Room authority missing", "Peer admission missing", "Authority not ready",
        "Seat or table revision changed", "Reservation not committed", "Authorization changed or expired", "Room proposal busy"};
    return std::string("Helper: ") + (view.helperReady ? "Ready" : "Unavailable") +
        " | Room: " + label(view.room,rooms,5) + " | Match: " + label(view.match,matches,5) +
        " | Control: " + label(view.control,health,4) + " | Gameplay: " + label(view.gameplay,health,4) +
        " | Verification: " + (view.verificationAvailable ? "Available" : "Unavailable") +
        " | Network check: " + label(view.probeState,probes,7) + (view.benchmark ? " (30s benchmark)" : " (5s check)") +
        (view.probeState==4 ? std::string(" | Check rejection: ") + label(view.probeFailure,probeFailures,12) : std::string()) +
        " | Measured route: " + label(view.probeRoute,routes,3) +
        " | Sent/scheduled: " + std::to_string(view.sent) + "/" + std::to_string(view.expected) +
        " | Replies/missed: " + std::to_string(view.replies) + "/" + std::to_string(view.missed) +
        " | RTT p50/p95/p99 us: " + std::to_string(view.p50Us) + "/" + std::to_string(view.p95Us) + "/" + std::to_string(view.p99Us) +
        " | RTT variation us: " + std::to_string(view.jitterUs) +
        " | Gameplay direct/relay links: " + std::to_string(view.directLinks) + "/" + std::to_string(view.relayedLinks) +
        " | Route changes/local drops/send pressure: " + std::to_string(view.routeChanges) + "/" + std::to_string(view.localDrops) + "/" + std::to_string(view.sendPressure);
}
ApplicationServices::ApplicationServices(std::wstring diagnosticsDirectory) : diagnosticsDirectory_(std::move(diagnosticsDirectory)), worker_(&ApplicationServices::Run, this) {}
ApplicationServices::~ApplicationServices() {
    Cancel();
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    wake_.notify_one(); worker_.join();
}
ServiceSnapshot ApplicationServices::Snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return state_; }
void ApplicationServices::Observe(const DiagnosticsView& diagnostics) {
    const auto description = DescribeDiagnostics(diagnostics);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = state_.connectionHistory;
    if (!history.empty() && history.back() == description) return;
    if (history.size() == 16) history.erase(history.begin());
    history.push_back(description);
}
bool ApplicationServices::Request(ServiceAction action, const DiagnosticsView& diagnostics) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ || state_.pending || action == ServiceAction::None) return false;
    cancelled_ = false;
    state_.downloadedBytes = state_.totalBytes = 0;
    request_ = action; diagnostics_ = diagnostics; state_.pending = true; state_.lastAction = action;
    state_.message = action == ServiceAction::OpenCommunity ? loc::T("services.opening_community") :
        action == ServiceAction::CheckUpdates ? loc::T("services.checking") :
        action == ServiceAction::ExportDiagnostics ? loc::T("services.exporting") :
        action == ServiceAction::InstallUpdate ? loc::T("services.downloading") : loc::T("services.opening_updater");
    wake_.notify_one(); return true;
}
void ApplicationServices::Run() {
    for (;;) {
        ServiceAction action; DiagnosticsView diagnostics; ServiceSnapshot next;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || request_ != ServiceAction::None; });
            if (stop_) return;
            action = request_; request_ = ServiceAction::None; diagnostics = diagnostics_; next = state_;
        }
        try {
            if (action == ServiceAction::CheckUpdates) {
                next.update = launcher::CheckForUpdate();
                next.message = !next.update.ok ? next.update.error : next.update.updateAvailable ?
                    loc::Tf("services.update_available",next.update.latestVersion) : loc::T("services.up_to_date");
            } else if (action == ServiceAction::ExportDiagnostics) {
                const auto directory = diagnosticsDirectory_.empty() ? std::filesystem::path(netplay::SettingsStore::DefaultDirectory()) / L"diagnostics" : std::filesystem::path(diagnosticsDirectory_);
                std::filesystem::create_directories(directory);
                const auto path = directory / L"ember-diagnostics.txt";
                std::ofstream output(path, std::ios::trunc);
                output << "SF4 Ember Netplay\nVersion: " << SF4E_APP_VERSION
                    << "\nTransport: Iroh / GGPO\n" << DescribeDiagnostics(diagnostics)
                    << "\nPing: " << (diagnostics.pingMs < 0 ? "Unavailable" : std::to_string(diagnostics.pingMs) + " ms")
                    << "\nSelected input delay: " << (diagnostics.selectedDelay<0?"Unavailable":
                        std::to_string(diagnostics.selectedDelay)+" frames") << '\n';
                if(diagnostics.performanceEnabled) {
                    output << "CPU work since latest match diagnostics reset (not displayed FPS):\n";
                    const char* names[]={"Complete outer call","Outer tick","Room runtime","Session-client step","Session-server step","GGPO idle",
                        "Rollback callback","Save state","Load state","Pacing wait","Diagnostic enqueue","Trace enqueue",
                        "Free state","Effect restore","VFX restore"};
                    static_assert(sizeof(names)/sizeof(names[0]) == DiagnosticTimingCount, "diagnostic timing labels must stay fixed");
                    for(std::size_t i=0;i<DiagnosticTimingCount;++i) {
                        const auto& t=diagnostics.timings[i];
                        output << names[i] << ": samples=" << t.count << " mean_ms=" << t.meanMs
                            << " max_ms=" << t.maxMs << " over_25ms=" << t.over25Ms << '\n';
                    }
                    output << "Rollback callbacks: " << diagnostics.rollbackCallbacks
                        << " | Prediction stalls: " << diagnostics.predictionStalls
                        << " | Prediction-skipped frames: " << diagnostics.predictionSkippedFrames << '\n';
                } else output << "CPU timing: unavailable (launch with rollback diagnostics enabled).\n";
                output << "Async logs: dropped=" << diagnostics.logDropped << '\n';
                output << "Lifecycle trace: dropped=" << diagnostics.traceDropped
                    << " last_write_ms=" << diagnostics.traceLastWriteMs << '\n';
                output << "Recovery checkpoint builds (current room lifetime): "
                    << (diagnostics.recoveryCheckpointBuildsAvailable ? std::to_string(diagnostics.recoveryCheckpointBuilds) : "Unavailable") << '\n';
                output << "Recent connection transitions (oldest first):\n";
                for (const auto& event : next.connectionHistory) output << event << '\n';
                output.close();
                next.message = output ? loc::T("services.diagnostics_saved") : loc::T("services.diagnostics_failed");
            } else if (action == ServiceAction::OpenUpdater || action == ServiceAction::OpenRecovery) {
                wchar_t root[MAX_PATH] = {};
                if (!install::GetInstallRoot(root, MAX_PATH)) throw std::runtime_error("install directory");
                const auto executable = std::filesystem::path(root) / L"Launcher.exe";
                const char* tag = loc::Tag(loc::Active());  // ASCII, so widening is byte for byte.
                std::wstring command = L"\"" + executable.wstring() + L"\" " +
                    (action == ServiceAction::OpenRecovery ? L"--recovery" : L"--updates") + L" --wait-pid " + std::to_wstring(GetCurrentProcessId()) +
                    L" --locale " + std::wstring(tag, tag + std::strlen(tag));
                STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
                if (!CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, root, &startup, &process)) {
                    next.message = loc::T("services.updater_start_failed");
                } else {
                    CloseHandle(process.hThread); CloseHandle(process.hProcess);
                    next.closeGame = true; next.message = loc::T("services.game_closing");
                }
            } else if (action == ServiceAction::OpenCommunity) {
                // ShellExecute may hand the URL to a COM-based handler; give it an apartment.
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                const std::wstring url = L"https://" + std::wstring(CommunityInvite, CommunityInvite + std::strlen(CommunityInvite));  // ASCII
                const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
                if (SUCCEEDED(com)) CoUninitialize();
                next.message = opened ? loc::T("services.community_opened") : loc::Tf("services.community_failed", CommunityInvite);
            } else if (action == ServiceAction::InstallUpdate) {
                if (!next.update.ok || !next.update.updateAvailable || next.update.expectedSha256.size() != 64) {
                    next.message = loc::T("services.no_verified_update");
                } else {
                    const auto result = launcher::DownloadAndApplyUpdate(next.update.zipDownloadUrl.c_str(), next.update.zipApiUrl.c_str(),
                        next.update.latestVersion.c_str(), next.update.expectedSha256.c_str(),
                        [&](std::uint64_t received, std::uint64_t total) {
                            if (cancelled_) return false;
                            next.downloadedBytes = received; next.totalBytes = total;
                            std::lock_guard<std::mutex> lock(mutex_);
                            state_.downloadedBytes = received; state_.totalBytes = total;
                            return true;
                        });
                    next.installed = result.ok;
                    next.message = result.ok ? loc::T("services.update_prepared") : result.error;
                }
            }
        } catch (...) { next.message = loc::T("services.operation_failed"); }
        if (cancelled_ && !next.installed) next.message = loc::T("services.operation_cancelled");
        next.pending = false;
        { std::lock_guard<std::mutex> lock(mutex_); next.connectionHistory = std::move(state_.connectionHistory); state_ = std::move(next); }
    }
}
} }
