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
    bool QueueOverlay(nlohmann::json snapshot);
    bool QueueLauncher(nlohmann::json snapshot);
    Status GetStatus() const;
    void Stop();

private:
    void Run();
    bool Queue(std::size_t section, nlohmann::json snapshot);
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
