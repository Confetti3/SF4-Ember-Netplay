#include "SettingsWriter.hxx"
#include <chrono>
#include <utility>

namespace sf4e { namespace netplay {

SettingsWriter::SettingsWriter(std::wstring directory) : store_(std::move(directory)), worker_([this] { Run(); }) {}
SettingsWriter::~SettingsWriter() { Stop(); }

bool SettingsWriter::QueueOverlay(nlohmann::json snapshot) { return Queue(0, std::move(snapshot)); }
bool SettingsWriter::QueueLauncher(nlohmann::json snapshot) { return Queue(1, std::move(snapshot)); }

bool SettingsWriter::Queue(std::size_t section, nlohmann::json snapshot) {
    try {
        // Bound copied UI data before it enters the worker's pending slot.
        if (!snapshot.is_object() || snapshot.dump().size() > 256 * 1024) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        pending_[section] = std::move(snapshot);
        ++submitted_[section];
        wake_.notify_one();
        return true;
    } catch (...) { return false; }
}

SettingsWriter::Status SettingsWriter::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {submitted_ != saved_, error_};
}

void SettingsWriter::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        wake_.notify_one();
    }
    if (worker_.joinable()) worker_.join();
}

void SettingsWriter::Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stopping_ || submitted_ != saved_; });
        if (submitted_ == saved_) return;
        const auto revision = submitted_;
        std::string batchError;
        for (std::size_t section = 0; section < pending_.size(); ++section) {
            if (revision[section] == saved_[section]) continue;
            auto value = pending_[section];
            // Capture the exact revision copied, including an edit arriving
            // while the other section was being written.
            const auto copiedRevision = submitted_[section];
            lock.unlock();
            std::string error;
            const bool saved = section == 0 ? store_.SaveOverlay(value, error) : store_.SaveLauncher(value, error);
            lock.lock();
            if (saved) saved_[section] = copiedRevision;
            else if (batchError.empty()) batchError = std::move(error);
        }
        error_ = std::move(batchError);
        // On shutdown attempt the final accepted snapshot once. A failed final
        // flush stays visible to the owner; it cannot silently claim persistence.
        if (stopping_ && revision == submitted_) return;
        if (saved_ != submitted_ && revision == submitted_) {
            wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_ || submitted_ != revision; });
        }
    }
}

} }
