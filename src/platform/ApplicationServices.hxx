#pragma once
#include "../launcher/update/github_release_client.hxx"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

namespace sf4e { namespace platform {
enum class ServiceAction { None, CheckUpdates, ExportDiagnostics, OpenUpdater, InstallUpdate, OpenRecovery };
struct DiagnosticsView {
    int room = 0, match = 0, control = 0, gameplay = 0, pingMs = -1;
    bool helperReady = false, verificationAvailable = false;
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
