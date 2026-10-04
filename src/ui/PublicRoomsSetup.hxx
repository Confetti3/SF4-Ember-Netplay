#pragma once
#include "../netplay/IdentityRequest.hxx"
#include <cstdint>
#include <optional>
#include <string>

namespace sf4e { namespace ui {
struct ShellView;

// The one-press setup of public rooms: where it stands, and why a failed one stopped.
// Checking, Creating, Finding and Trusting are its running steps; the others end it.
enum class PublicSetupStep { None, Checking, Creating, Finding, Trusting, Done, NeedsId, Failed };
struct PublicSetupView {
    PublicSetupStep step = PublicSetupStep::None;
    std::string failure;
    bool Running() const { return step >= PublicSetupStep::Checking && step <= PublicSetupStep::Trusting; }
};

// The setup's own sequence: read the Ember ID, create it when nothing must be
// typed, read the trusted services, look Ember's own service up and trust it,
// then read the services back. It asks one request at a time and moves only on
// the answers to its own requests, which the Ember ID panel hands it by their
// owner; what the screens or Connect Discord asked meanwhile never reaches it.
// The player agreed to Ember's own service in the setup's dialog, so a service
// they removed earlier is trusted again, and a service at any other address never is.
class PublicRoomsSetup {
public:
    PublicSetupView View() const { return view_; }
    bool Running() const { return view_.Running(); }
    // The run its requests belong to: a new start or a stop retires an answer still out.
    std::uint64_t Run() const { return run_; }
    // Starts over from the Ember ID's status, its first request.
    netplay::IdentityRequest Start();
    // Ember is hidden: the run ends, and an answer still out is no longer its own.
    void Stop() { view_ = {}; ++run_; }
    // The end (Done, NeedsId or Failed) has been shown.
    void Clear() { if (!Running()) view_ = {}; }
    // The player left public rooms: an end nobody is looking at is not carried to the next visit.
    void Unseen() { if (view_.step == PublicSetupStep::NeedsId || view_.step == PublicSetupStep::Failed) view_ = {}; }
    // Its request was answered: the next one to ask, if any. `op` is the request's.
    std::optional<netplay::IdentityRequest> Answered(const ShellView& view, netplay::IdentityOp op);
    // Its request failed with the helper's or the bridge's code, `general` being
    // the Ember ID screens' words for it; or stopped with words of its own.
    void Failed(const std::string& code, std::string general);
    void Fail(std::string text);
    // The trusted service the setup found, once Done.
    const std::string& Service() const { return service_; }
private:
    PublicSetupView view_;
    std::uint64_t run_ = 0;
    std::string service_;
};
} }
