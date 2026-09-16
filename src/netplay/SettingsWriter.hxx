#pragma once
#include "SettingsStore.hxx"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <array>

namespace sf4e { namespace netplay {

// One pending complete snapshot per consumer (overlay and launcher/profile).
// Rapid edits replace older pending edits; no game objects cross threads.
class SettingsWriter {
public:
    struct Status {
        bool pending = false;
        std::string error;
    };
    explicit SettingsWriter(std::wstring directory);
    ~SettingsWriter();
    // Returns the accepted revision (never 0), or 0 when the snapshot was
    // rejected. Acceptance is not persistence: a revision is on disk only once
    // the matching Saved*Revision() reaches it. Snapshots are complete, so a
    // later saved revision also persists every earlier one.
    std::uint64_t QueueOverlay(nlohmann::json snapshot);
    std::uint64_t QueueLauncher(nlohmann::json snapshot);
    std::uint64_t SavedOverlayRevision() const;
    std::uint64_t SavedLauncherRevision() const;
    Status GetStatus() const;
    void Stop();

private:
    void Run();
    std::uint64_t Queue(std::size_t section, nlohmann::json snapshot);
    SettingsStore store_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::array<nlohmann::json, 2> pending_;
    std::array<std::uint64_t, 2> submitted_ = {};
    std::array<std::uint64_t, 2> saved_ = {};
    bool stopping_ = false;
    std::string error_;
    std::thread worker_;
};

} }
