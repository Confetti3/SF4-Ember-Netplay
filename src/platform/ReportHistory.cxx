#include "ReportHistory.hxx"
#include "DurableFile.hxx"
#include "../common/BoundedRead.hxx"
#include "../netplay/SettingsStore.hxx"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <thread>

namespace sf4e { namespace reports {
namespace {
// The record's lock file, held for the whole of a change.
class HistoryLock {
public:
    HistoryLock(const std::filesystem::path& record, std::chrono::milliseconds wait) {
        std::error_code ec; std::filesystem::create_directories(record.parent_path(), ec);
        const auto path = std::filesystem::path(record).concat(L".lock");
        const auto until = std::chrono::steady_clock::now() + wait;
        for (;;) {
            handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE || std::chrono::steady_clock::now() >= until) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~HistoryLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    HistoryLock(const HistoryLock&) = delete;
    HistoryLock& operator=(const HistoryLock&) = delete;
    bool Held() const { return handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
History Read(const std::filesystem::path& path) {
    History history;
    if (path.empty()) { history.readable = false; return history; }
    const auto read = durable::ReadBounded(path, SentFileLimit);
    if (read.status == durable::ReadStatus::Missing) return history;
    const auto parsed = read.status == durable::ReadStatus::Read ?
        ParseSent(std::string_view(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size())) : std::nullopt;
    if (parsed) history.sent = *parsed; else history.readable = false;
    return history;
}
std::string AttemptCode() {
    unsigned char random[8] = {};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    static const char digits[] = "0123456789abcdef";
    std::string code;
    for (const unsigned char b : random) { code += digits[b >> 4]; code += digits[b & 15]; }
    return code;
}
}
ReportHistory::ReportHistory(std::filesystem::path path, Publish publish, std::chrono::milliseconds lockWait)
    : path_(std::move(path)), publish_(std::move(publish)), lockWait_(lockWait) {
    if (!publish_) publish_ = [](const std::filesystem::path& path, const std::string& text) {
        return static_cast<bool>(durable::PublishReplace(path, durable::PartialPath(path), text.data(), text.size()));
    };
}
History ReportHistory::Load() const { return Read(path_); }
Reservation ReportHistory::Reserve(Record& attempt, bool automatic, std::int64_t now, History* history) {
    if (path_.empty()) { if (history) *history = Read(path_); return Reservation::Unreadable; }
    HistoryLock lock(path_, lockWait_);
    auto current = Read(path_);
    if (history) *history = current;
    if (!lock.Held()) return Reservation::Failed;
    if (!current.readable) return Reservation::Unreadable;
    if (automatic && !AutomaticAllowed(current.sent, now)) return Reservation::LimitReached;
    attempt.status = Status::Pending; attempt.time = now; attempt.automatic = automatic;
    attempt.attempt = AttemptCode();
    if (attempt.attempt.empty()) return Reservation::Failed;
    auto next = current.sent;
    Append(next, attempt);
    if (!publish_(path_, SentJson(next))) return Reservation::Failed;
    if (history) history->sent = std::move(next);
    return Reservation::Reserved;
}
bool ReportHistory::Complete(const Record& outcome, History* history) {
    if (path_.empty() || outcome.attempt.empty()) { if (history) *history = Read(path_); return false; }
    HistoryLock lock(path_, lockWait_);
    auto current = Read(path_);
    if (history) *history = current;
    if (!lock.Held() || !current.readable) return false;
    auto next = current.sent;
    const auto found = std::find_if(next.begin(), next.end(), [&](const Record& r) { return r.attempt == outcome.attempt; });
    // The reservation keeps its time, so the day's count is unchanged.
    if (found != next.end()) { const auto time = found->time; *found = outcome; found->time = time; }
    else Append(next, outcome);
    if (!publish_(path_, SentJson(next))) return false;
    if (history) history->sent = std::move(next);
    return true;
}
std::filesystem::path SentReportsPath() {
    const auto directory = netplay::SettingsStore::DefaultDirectory();
    return directory.empty() ? std::filesystem::path() : std::filesystem::path(directory) / L"sent-reports.json";
}
} }
