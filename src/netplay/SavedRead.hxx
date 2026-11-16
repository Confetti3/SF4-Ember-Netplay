#pragma once

#include "SettingsStore.hxx"

#include <chrono>
#include <cstddef>
#include <future>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// The one owner of whether this run has read the player's saved settings. The
// first read is made at once, where the program starts; one that fails for
// a reason that may pass (another process holds the settings lock, the file is
// being scanned or written) is tried again after a short wait, off the calling
// thread, each wait longer than the last, a few times. It settles as one of:
//   Read     the settings were read (saved()).
//   Damaged  the file was not valid JSON: it has been set aside and a new one
//            started (keptAside()); the player starts from defaults.
//   GaveUp   the settings could not be read: nothing may be written this run.
// The caller polls from its own tick, never waits, and acts on the settling
// once (Poll's true).
class SavedRead {
public:
    using Clock = std::chrono::steady_clock;
    enum class State { Pending, Read, Damaged, GaveUp };

    // The waits before each new try: 1, 2, 4, 8 and 16 s.
    static std::vector<Clock::duration> DefaultRetries();

    // Reads now, on this thread, as the program's start does.
    SavedRead(SettingsStore store, Clock::time_point now, std::vector<Clock::duration> retries = DefaultRetries());
    SavedRead(const SavedRead&) = delete;
    SavedRead& operator=(const SavedRead&) = delete;

    State state() const { return state_; }
    // Starts a try that is due and takes one that has finished. True on the call
    // that settles the read; false while it is pending and after.
    bool Poll(Clock::time_point now);

    // The launcher view of the settings (SettingsStore::LoadLauncher), once Read.
    const nlohmann::json& saved() const { return saved_; }
    // The last failed try: why, in words for the log, and who holds the lock if
    // that could be told. Empty when no try has failed.
    ReadFailure failure() const { return failure_; }
    const std::string& error() const { return error_; }
    const std::string& lockHolder() const { return holder_; }
    int failedTries() const { return failedTries_; }
    // The name the damaged file was kept under, in the settings folder.
    const std::wstring& keptAside() const { return keptAside_; }

private:
    struct Attempt {
        bool ok = false;
        nlohmann::json saved;
        ReadFailure why = ReadFailure::None;
        std::string error, holder;
        std::wstring keptAside;
    };
    static Attempt Try(const SettingsStore& store);
    void Take(Attempt attempt, Clock::time_point now);

    SettingsStore store_;
    std::vector<Clock::duration> retries_;
    std::size_t retried_ = 0;
    Clock::time_point due_;
    std::future<Attempt> running_;
    State state_ = State::Pending;
    nlohmann::json saved_ = nlohmann::json::object();
    ReadFailure failure_ = ReadFailure::None;
    std::string error_, holder_;
    int failedTries_ = 0;
    std::wstring keptAside_;
};

} }
