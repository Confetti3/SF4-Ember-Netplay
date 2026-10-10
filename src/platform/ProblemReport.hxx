#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "../common/UpdateChannel.hxx"

namespace sf4e { namespace reports {
constexpr std::size_t LogLimit = 192 * 1024;
constexpr std::size_t DumpLimit = 4 * 1024 * 1024;
constexpr std::size_t BodyLimit = 5 * 1024 * 1024;
struct Metadata {
    std::string kind = "problem", appVersion, channel, buildId, sourceRevision, windowsVersion;
    std::optional<std::uint32_t> exitCode, exceptionCode;
    std::optional<std::uint64_t> crashAddress;
};
struct Log { std::string name, text; };
struct Report {
    Metadata meta;
    std::vector<Log> logs;
    std::string minidump;
};
struct CrashContext {
    std::optional<std::uint32_t> exitCode, exceptionCode;
    std::optional<std::uint64_t> address;
    std::filesystem::path smallDump;
    std::filesystem::file_time_type started = (std::filesystem::file_time_type::max)();
};
// A report the player sends from its preview. The small dump goes only with
// the player's tick for this report, so it is off until set. A report sent
// without a press is not a Submission (ReportWorkflow.hxx: AutomaticCrash).
struct Submission {
    std::string comment;
    bool includeDump = false;
    std::shared_ptr<const Report> preview;
};
struct Multipart { std::string contentType, body; };
// The player's Windows account and Ember name, read on the report worker:
// the user name, the profile folder's name in its long and 8.3 forms, the PC
// name and the player name. Each that stands alone in a log line becomes
// <user>, however short, under Unicode case folding and in any script, written
// plainly or inside a JSON string (escaped quotes and backslashes, and
// four-digit hex escapes).
struct Account { std::vector<std::string> names; };
std::string RedactLine(std::string_view line, const Account& account = {});
std::string Utf8Tail(std::string_view bytes, std::size_t limit = LogLimit);
std::size_t CharacterCount(std::string_view text);
nlohmann::json MetaJson(const Metadata& meta, const std::string& comment);
bool BuildMultipart(const Report& report, const Submission& submission, const std::string& boundary,
    Multipart& result, std::string& error);
// The intake's name for the channel this build updates from.
const char* ReportChannel(updates::UpdateChannel channel);
// `afterRead` runs after each log's tail is read; tests change the log there.
Report Collect(const std::filesystem::path& logsDirectory, Metadata meta, const CrashContext& crash,
    const std::function<bool()>& cancelled, const Account& account = {},
    const std::function<void(const std::filesystem::path&)>& afterRead = {});

// The record of the reports this PC sent (ReportHistory.hxx). Pending: a
// send was reserved and has not finished, as when its upload was cut off.
// Received: the intake stored it. Refused: it answered, but not with 202.
// NotSent: no answer came, such as no connection or a cancel.
enum class Status { Pending, Received, Refused, NotSent };
struct Record {
    std::int64_t time = 0;  // seconds since 1970, UTC, when the send was reserved
    Status status = Status::Pending;
    // kind: "crash" or "problem"; id: 32 lowercase hex digits or empty;
    // reason: a short code of a-z, 0-9 and '_', or empty; attempt: 16
    // lowercase hex digits that tie the outcome to its reservation.
    std::string kind = "crash", id, reason, attempt;
    bool dump = false;       // the small dump went with it
    bool automatic = false;  // it went without a press
};
constexpr std::size_t SentKept = 50;
constexpr std::size_t SentFileLimit = 64 * 1024;
// Oldest first, at most SentKept.
void Append(std::vector<Record>& sent, Record record);
std::string SentJson(const std::vector<Record>& sent);
// The record as this code writes it, or nothing when the text is anything
// else: a damaged record is never read as a shorter one.
std::optional<std::vector<Record>> ParseSent(std::string_view text);

// With Send problem reports on, at most this many reports a day go without a
// press, so a game that crashes at every start cannot keep sending. Every
// report sent in the last day counts, pending ones too, and so does one dated
// after `now`.
constexpr int AutomaticPerDay = 3;
bool AutomaticAllowed(const std::vector<Record>& sent, std::int64_t now);
} }
