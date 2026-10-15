#pragma once
#include "ProblemReport.hxx"
#include "ReportHistory.hxx"
#include "../common/sf4e__NetUtil.hxx"
#include <chrono>
#include <filesystem>
#include <memory>
#include <variant>
#include <vector>

namespace sf4e { namespace reports {
enum class Phase { Idle, Preparing, Preview, Submitting, Sent, Retry, Failed, Cancelled };
struct WorkflowState {
    Phase phase = Phase::Idle;
    std::uint64_t preparation = 0;
    std::shared_ptr<const Report> preview;
    std::string id, message;
    std::chrono::steady_clock::time_point retryAt{};
    // How the last send went, once it reached the network.
    std::optional<Record> record;
    // The last operation was a crash report sent without a press, and it was
    // allowed: the setting is on and the send is counted.
    bool automatic = false, authorized = false;
    // The crash's report is offered: no automatic send happened, or Always
    // send could not go. A preview keeps it; a submission ends it.
    bool offer = false;
    bool Busy() const { return phase == Phase::Preparing || phase == Phase::Submitting; }
    bool Succeeded() const { return phase == Phase::Preview || phase == Phase::Sent; }
};
struct Preparation {
    std::optional<CrashContext> crash;
    updates::UpdateChannel channel = updates::UpdateChannel::Stable;
};
// A crash's report sent without a press: logs only, no comment, no dump.
// Existing: Send problem reports was already on, which the worker checks.
// Confirmed: the player just confirmed Always send, which the worker saves
// before anything is sent.
struct AutomaticCrash {
    enum class Consent { Existing, Confirmed };
    Consent consent = Consent::Existing;
    CrashContext crash;
    updates::UpdateChannel channel = updates::UpdateChannel::Stable;
};
using Operation = std::variant<Preparation, Submission, AutomaticCrash>;
// What redaction needs to know about the player, or that it could not be read.
struct Identity {
    bool ok = false;
    Account account;
};
// Dependencies also let offline tests drive the complete production workflow.
struct WorkflowDependencies {
    std::function<Identity(const std::function<bool()>&)> identity;
    std::function<Report(const Preparation&, const Account&, const std::function<bool()>&)> collect;
    std::function<HttpPostResult(const Multipart&, const std::function<bool()>&)> upload;
    // Send problem reports as saved: nothing when the settings cannot be read.
    std::function<std::optional<bool>()> reportsOn;
    std::function<bool()> turnReportsOn;
    std::shared_ptr<ReportHistory> history;
    // Shows an automatic send as on its way once it is allowed and counted.
    std::function<void(const WorkflowState&)> progress;
};
class ReportWorkflow {
public:
    // Called while queueing on the service lock. A new preparation discards the
    // old preview; a submission must refer to the exact immutable preview.
    static bool Begin(const Operation& operation, WorkflowState& state);
    // Executes once on the existing services worker. Results keep retryable
    // previews intact and carry their own phase, receipt and error message.
    static WorkflowState Execute(const Operation& operation, WorkflowState state,
        const std::function<bool()>& cancelled, const WorkflowDependencies& dependencies = {});
};
// Send problem reports (netplay/BoolPreferences.hxx) as saved, or nothing when
// the settings cannot be read; TurnReportsOn saves it on, as Always send asks.
std::optional<bool> ReportsOn();
bool TurnReportsOn();
// The player name from the settings in `settingsDirectory` and the Windows
// account. Settings busy in another process are tried again until `wait` is
// spent; anything still unread fails the whole identity, so a report is
// never prepared without the names it must remove.
Identity CollectIdentity(const std::wstring& settingsDirectory, const std::function<bool()>& cancelled,
    std::chrono::milliseconds wait = std::chrono::milliseconds(3000));
} }
