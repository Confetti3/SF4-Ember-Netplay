#include "SavedRead.hxx"

#include <utility>

namespace sf4e { namespace netplay {

std::vector<SavedRead::Clock::duration> SavedRead::DefaultRetries() {
    using std::chrono::seconds;
    return {seconds(1), seconds(2), seconds(4), seconds(8), seconds(16)};
}

SavedRead::SavedRead(SettingsStore store, Clock::time_point now, std::vector<Clock::duration> retries)
    : store_(std::move(store)), retries_(std::move(retries)), due_(now) {
    Take(Try(store_), now);
}

SavedRead::Attempt SavedRead::Try(const SettingsStore& store) {
    Attempt attempt;
    attempt.ok = store.LoadLauncher(attempt.saved, attempt.error, &attempt.why);
    if (attempt.ok) return attempt;
    if (attempt.why == ReadFailure::Busy) attempt.holder = store.LockHolder();
    if (attempt.why == ReadFailure::Damaged) {
        std::string asideError;
        if (store.KeepDamagedAside(attempt.keptAside, asideError)) attempt.saved = nlohmann::json::object();
        else {
            attempt.error += " It could not be set aside: " + asideError;
            attempt.why = ReadFailure::Unreadable;
        }
    }
    return attempt;
}

void SavedRead::Take(Attempt attempt, Clock::time_point now) {
    if (attempt.ok) {
        saved_ = std::move(attempt.saved);
        state_ = State::Read;
        return;
    }
    failure_ = attempt.why;
    error_ = std::move(attempt.error);
    holder_ = std::move(attempt.holder);
    ++failedTries_;
    if (!attempt.keptAside.empty()) {
        keptAside_ = std::move(attempt.keptAside);
        state_ = State::Damaged;
    } else if (attempt.why == ReadFailure::Unsupported || retried_ >= retries_.size()) {
        // A file of a version this build does not take does not come right by waiting.
        state_ = State::GaveUp;
    } else {
        due_ = now + retries_[retried_++];
    }
}

bool SavedRead::Poll(Clock::time_point now) {
    if (state_ != State::Pending) return false;
    if (running_.valid()) {
        if (running_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        Take(running_.get(), now);
        return state_ != State::Pending;
    }
    if (now < due_) return false;
    const SettingsStore store = store_;
    running_ = std::async(std::launch::async, [store] { return Try(store); });
    return false;
}

} }
