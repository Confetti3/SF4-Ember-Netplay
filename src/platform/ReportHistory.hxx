#pragma once
#include "ProblemReport.hxx"
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The record of the reports this PC sent, sent-reports.json in the settings
// folder: the one place a send is counted. The launcher and the game each
// send, so every change holds a lock file across read, change and replace.
// A send is reserved before its upload starts and completed after it, so an
// upload cut off by a crash or a closed window still counts.
namespace sf4e { namespace reports {

// Missing history is an empty record. History that cannot be read is not:
// it never reads as fewer sends than there were.
struct History {
    bool readable = true;
    std::vector<Record> sent;
};
enum class Reservation {
    Reserved,
    // An automatic send, with the day's automatic sends used up.
    LimitReached,
    // The record cannot be read, so nothing is counted.
    Unreadable,
    // The lock or the write failed; nothing changed.
    Failed,
};

class ReportHistory {
public:
    // Writes the record whole or not at all; true when it was written.
    using Publish = std::function<bool(const std::filesystem::path&, const std::string&)>;
    explicit ReportHistory(std::filesystem::path path, Publish publish = {},
        std::chrono::milliseconds lockWait = std::chrono::milliseconds(2000));
    History Load() const;
    // Adds `attempt` as Pending, with a new attempt code, before its upload.
    // An automatic attempt is refused once the day's automatic sends are used
    // up. `history` gets the record as it is afterwards.
    Reservation Reserve(Record& attempt, bool automatic, std::int64_t now, History* history = nullptr);
    // Replaces the reservation with how the send went. False when the record
    // could not be changed, which leaves the reservation counted.
    bool Complete(const Record& outcome, History* history = nullptr);
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
    Publish publish_;
    std::chrono::milliseconds lockWait_;
};

// sent-reports.json in the settings folder, or empty without one.
std::filesystem::path SentReportsPath();
} }
