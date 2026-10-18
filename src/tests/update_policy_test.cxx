// The launch-time update check: the policy (cooldown, retry after a failure,
// skip, what is presented), the --install-offer handoff, and the Updates
// window's recheck of the accepted release. The release client is replaced, so
// nothing here reaches GitHub, and nothing opens the Launcher.
#include "../platform/ApplicationServices.hxx"
#include "../platform/UpdatePolicy.hxx"
#include "../launcher/update/InstallOffer.hxx"
#include "../common/Localization.hxx"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "test_support.hxx"
#include "temp_root.hxx"

using namespace sf4e;
using namespace sf4e::platform;
using launcher::InstallOffer;
using launcher::InstallOfferGate;
using launcher::UpdateChannel;
using launcher::UpdateCheckResult;

namespace {
const std::string DigestA(64, 'a'), DigestB(64, 'b');

UpdateCheckResult Release(const std::string& tag, const std::string& digest, bool available = true) {
    UpdateCheckResult result;
    result.ok = true; result.updateAvailable = available; result.latestVersion = tag; result.expectedSha256 = digest;
    result.zipDownloadUrl = "https://example.invalid/package.zip";
    return result;
}
UpdateCheckResult Failure() {
    UpdateCheckResult result;
    result.error = "GitHub could not be reached";
    return result;
}

// What the preferences file would hold, and how often it was written.
struct Memory {
    UpdatePreferences saved;
    int lastCheckWrites = 0, skipWrites = 0;
    bool failSkip = false;
    UpdatePolicy::Store Store() {
        return {[this] { return saved; },
            [this](std::uint64_t time) { saved.lastCheck = time; ++lastCheckWrites; return true; },
            [this](const std::string& tag) { if (failSkip) return false; saved.skipped = tag; ++skipWrites; return true; }};
    }
};

// A service that checks against `release` (never GitHub) at `now`, over `memory`.
struct Launch {
    std::atomic<int> checks{0};
    UpdateCheckResult release;
    std::uint64_t now = 1760000000;
    ServiceHooks Hooks(Memory& memory) {
        ServiceHooks hooks;
        hooks.checkForUpdate = [this](UpdateChannel) { ++checks; return release; };
        hooks.clock = [this] { return now; };
        hooks.policyStore = memory.Store();
        return hooks;
    }
};

ServiceSnapshot Settled(const ApplicationServices& service) {
    for (int i = 0; i < 500; ++i) {
        const auto snapshot = service.Snapshot();
        if (!snapshot.pending) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(false);
    return {};
}
}

int main() {
    const auto root = MakeTempRoot(L"ember-update-policy-test-");
    const std::uint64_t Hour = 3600;

    // The cooldown: six hours from the last check that reached GitHub, on by default.
    {
        Memory memory; UpdatePolicy policy(memory.Store());
        CHECK(policy.ShouldCheck(1760000000));
        memory.saved.lastCheck = 1760000000;
        CHECK(!policy.ShouldCheck(1760000000));
        CHECK(!policy.ShouldCheck(1760000000 + 6 * Hour - 1));
        CHECK(policy.ShouldCheck(1760000000 + 6 * Hour));
        // A clock set back does not hold the check off until it catches up.
        CHECK(policy.ShouldCheck(1760000000 - Hour));
        memory.saved.checkAtLaunch = false;
        CHECK(!policy.ShouldCheck(1760000000 + 100 * Hour));
    }
    // A check that failed leaves no time behind: the next launch tries again at once.
    {
        Memory memory; UpdatePolicy policy(memory.Store());
        policy.CheckFinished(false, 1760000000);
        CHECK(memory.lastCheckWrites == 0 && policy.ShouldCheck(1760000000 + 60));
        policy.CheckFinished(true, 1760000000);
        CHECK(memory.lastCheckWrites == 1 && !policy.ShouldCheck(1760000000 + 60));
    }
    // What is presented: a verifiable newer release that was not skipped.
    {
        Memory memory; UpdatePolicy policy(memory.Store());
        const auto offer = policy.Present(UpdateChannel::Nightly, Release("v1.3.0-nightly20261014", DigestA));
        CHECK(offer && offer->tag == "v1.3.0-nightly20261014" && offer->sha256 == DigestA && offer->channel == UpdateChannel::Nightly);
        CHECK(!policy.Present(UpdateChannel::Stable, Release("v1.3.0", DigestA, false)));
        CHECK(!policy.Present(UpdateChannel::Stable, Release("v1.3.0", "")));
        CHECK(!policy.Present(UpdateChannel::Stable, Release("v1.3.0", "abc")));
        CHECK(!policy.Present(UpdateChannel::Stable, Failure()));
        auto back = Release("v1.2.0", DigestA); back.goesBack = true;
        // Help & about may offer a step back the player chose; the launch question never does.
        CHECK(launcher::OfferFrom(UpdateChannel::Stable, back) && !policy.Present(UpdateChannel::Stable, back));
        CHECK(!policy.Present(UpdateChannel::Stable, Release("not a version", DigestA)));
        // Skipping a version skips that one; a newer release is asked about again.
        CHECK(policy.Skip("v1.3.0") && memory.saved.skipped == "v1.3.0");
        CHECK(!policy.Present(UpdateChannel::Stable, Release("v1.3.0", DigestA)));
        CHECK(policy.Present(UpdateChannel::Stable, Release("v1.3.1", DigestA)));
    }

    // The handoff: --install-offer <channel> <tag> <sha256>.
    {
        const auto parsed = launcher::ParseInstallOffer({"prerelease", "v1.3.0-rc1", std::string(64, 'A')});
        CHECK(parsed && parsed->channel == UpdateChannel::Beta && parsed->tag == "v1.3.0-rc1" && parsed->sha256 == std::string(64, 'a'));
        CHECK(launcher::ParseInstallOffer({"nightly", "v1.3.0-nightly20261014", DigestB}));
        const InstallOffer offer{UpdateChannel::Nightly, "v1.3.0-nightly20261014", DigestB};
        CHECK(launcher::InstallOfferArguments(offer) == L" --install-offer nightly v1.3.0-nightly20261014 " + std::wstring(DigestB.begin(), DigestB.end()));
        CHECK(launcher::ParseInstallOffer({"nightly", offer.tag, offer.sha256}) == offer);
        CHECK(!launcher::ParseInstallOffer({}));
        CHECK(!launcher::ParseInstallOffer(std::vector<std::string>{"stable", std::string("v1.3.0")}));
        CHECK(!launcher::ParseInstallOffer({"stable", "v1.3.0", DigestA, "extra"}));
        CHECK(!launcher::ParseInstallOffer({"beta", "v1.3.0", DigestA}));
        CHECK(!launcher::ParseInstallOffer({"Stable", "v1.3.0", DigestA}));
        CHECK(!launcher::ParseInstallOffer({"stable", "latest", DigestA}));
        CHECK(!launcher::ParseInstallOffer({"stable", "v1.3.0 & calc", DigestA}));
        CHECK(!launcher::ParseInstallOffer({"stable", "v1.3.0", std::string(63, 'a')}));
        CHECK(!launcher::ParseInstallOffer({"stable", "v1.3.0", std::string(64, 'g')}));
    }

    // The launch check through the service, over a fake release client.
    {
        Memory memory; Launch launch; launch.release = Release("v1.3.0", DigestA);
        {
            ApplicationServices service(root.wstring(), launch.Hooks(memory));
            CHECK(service.Snapshot().launchCheck == LaunchCheck::Waiting && !service.Snapshot().launchOffer);
            CHECK(service.RequestLaunchCheck());
            const auto done = Settled(service);
            CHECK(done.launchCheck == LaunchCheck::Settled && launch.checks == 1);
            CHECK(done.launchOffer && done.launchOffer->tag == "v1.3.0" && done.launchOffer->sha256 == DigestA && done.launchOffer->channel == done.channel);
            CHECK(memory.saved.lastCheck == launch.now);
            // Once per launch, however often the shell asks.
            CHECK(service.RequestLaunchCheck() && service.Request(ServiceAction::LaunchUpdateCheck));
            CHECK(Settled(service).launchCheck == LaunchCheck::Settled && launch.checks == 1);
            // The offer the service holds is the only one it will act on.
            CHECK(!service.UpdateNow({done.channel, "v9.9.9", DigestA}) && !service.SkipUpdate({done.channel, "v1.3.0", DigestB}));
            CHECK(!service.Request(ServiceAction::UpdateNow) && !service.Request(ServiceAction::SkipUpdate));
            // Skipping it is saved and takes the offer back.
            CHECK(service.SkipUpdate(*done.launchOffer));
            const auto skipped = Settled(service);
            CHECK(skipped.succeeded && !skipped.launchOffer && memory.saved.skipped == "v1.3.0" && memory.skipWrites == 1);
        }
        // The next launch, inside the cooldown: no check, nothing to ask.
        {
            launch.now += 2 * Hour;
            ApplicationServices service(root.wstring(), launch.Hooks(memory));
            CHECK(service.RequestLaunchCheck());
            const auto done = Settled(service);
            CHECK(done.launchCheck == LaunchCheck::Settled && !done.launchOffer && launch.checks == 1);
        }
        // After it: the check runs, and the skipped version is not offered again, a newer one is.
        {
            launch.now += 6 * Hour;
            ApplicationServices service(root.wstring(), launch.Hooks(memory));
            CHECK(service.RequestLaunchCheck());
            const auto done = Settled(service);
            CHECK(launch.checks == 2 && !done.launchOffer && memory.saved.lastCheck == launch.now);
        }
        {
            launch.now += 6 * Hour; launch.release = Release("v1.3.1", DigestB);
            ApplicationServices service(root.wstring(), launch.Hooks(memory));
            CHECK(service.RequestLaunchCheck());
            const auto done = Settled(service);
            CHECK(launch.checks == 3 && done.launchOffer && done.launchOffer->tag == "v1.3.1");
        }
    }
    // A failed check offers nothing, leaves the cooldown alone, and the next launch retries.
    {
        Memory memory; Launch launch; launch.release = Failure();
        {
            ApplicationServices service(root.wstring(), launch.Hooks(memory));
            CHECK(service.RequestLaunchCheck());
            const auto done = Settled(service);
            CHECK(done.launchCheck == LaunchCheck::Settled && !done.launchOffer && launch.checks == 1 && memory.lastCheckWrites == 0);
        }
        launch.now += 60; launch.release = Release("v1.3.0", DigestA);
        ApplicationServices service(root.wstring(), launch.Hooks(memory));
        CHECK(service.RequestLaunchCheck());
        const auto done = Settled(service);
        CHECK(launch.checks == 2 && done.launchOffer && memory.lastCheckWrites == 1);
    }
    // A skip that cannot be saved keeps the offer, says why, and can be tried again.
    {
        Memory memory; memory.failSkip = true; Launch launch; launch.release = Release("v1.3.0", DigestA);
        ApplicationServices service(root.wstring(), launch.Hooks(memory));
        CHECK(service.RequestLaunchCheck());
        const auto found = Settled(service);
        CHECK(found.launchOffer && found.skipFailures == 0 && found.skipError.empty());
        CHECK(service.SkipUpdate(*found.launchOffer));
        const auto failed = Settled(service);
        CHECK(!failed.succeeded && failed.launchOffer == found.launchOffer && failed.skipFailures == 1);
        CHECK(!failed.skipError.empty() && failed.skipError == loc::T("common.save_failed") && failed.message == failed.skipError);
        CHECK(memory.saved.skipped.empty() && memory.skipWrites == 0);
        memory.failSkip = false;
        CHECK(service.SkipUpdate(*found.launchOffer));
        const auto saved = Settled(service);
        CHECK(saved.succeeded && !saved.launchOffer && saved.skipFailures == 1 && memory.saved.skipped == "v1.3.0" && memory.skipWrites == 1);
    }
    // A skip refused while another operation runs is counted, and the offer stays.
    {
        Memory memory; Launch launch; launch.release = Release("v1.3.0", DigestA);
        std::atomic<bool> hold{false};
        auto hooks = launch.Hooks(memory);
        const auto check = hooks.checkForUpdate;
        hooks.checkForUpdate = [&hold, check](UpdateChannel channel) {
            while (hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return check(channel);
        };
        ApplicationServices service(root.wstring(), hooks);
        CHECK(service.RequestLaunchCheck());
        const auto found = Settled(service);
        CHECK(found.launchOffer);
        hold = true;
        CHECK(service.Request(ServiceAction::CheckUpdates) && service.Snapshot().pending);
        CHECK(!service.SkipUpdate(*found.launchOffer));
        const auto refused = service.Snapshot();
        CHECK(refused.skipFailures == 1 && !refused.skipError.empty() && refused.skipError == loc::T("runtime.operation_busy") && memory.skipWrites == 0);
        hold = false;
        const auto after = Settled(service);
        CHECK(after.skipFailures == 1 && after.skipError == refused.skipError && after.launchOffer == found.launchOffer);
        CHECK(service.SkipUpdate(*found.launchOffer));
        const auto saved = Settled(service);
        CHECK(saved.succeeded && !saved.launchOffer && memory.saved.skipped == "v1.3.0");
    }
    // Turned off in Settings: no check at all.
    {
        Memory memory; memory.saved.checkAtLaunch = false; Launch launch; launch.release = Release("v1.3.0", DigestA);
        ApplicationServices service(root.wstring(), launch.Hooks(memory));
        CHECK(service.RequestLaunchCheck());
        const auto done = Settled(service);
        CHECK(done.launchCheck == LaunchCheck::Settled && !done.launchOffer && launch.checks == 0);
    }

    // The Updates window rechecks what the game offered. The game's offer was v1.3.0 / A.
    {
        const auto windowSees = [&](const UpdateCheckResult& found, bool& installs, bool& changed) {
            Memory memory; Launch launch; launch.release = found;
            ApplicationServices service(root.wstring(), launch.Hooks(memory));
            CHECK(service.Request(ServiceAction::CheckUpdates));
            const auto state = Settled(service);
            InstallOfferGate gate(InstallOffer{state.channel, "v1.3.0", DigestA});
            // Until the window's own check has finished, nothing is decided.
            CHECK(gate.Observe(state.channel, state.update, false) == InstallOfferGate::Step::Wait);
            const auto step = gate.Observe(state.channel, state.update, !state.pending && state.lastAction == ServiceAction::CheckUpdates);
            installs = step == InstallOfferGate::Step::Install; changed = step == InstallOfferGate::Step::Changed;
            // It decides once.
            CHECK(gate.Observe(state.channel, state.update, true) == InstallOfferGate::Step::Wait);
            CHECK(step != InstallOfferGate::Step::Wait);
        };
        bool installs = false, changed = false;
        windowSees(Release("v1.3.0", DigestA), installs, changed);
        CHECK(installs && !changed);
        // A newer release appeared between accepting and the launcher starting.
        windowSees(Release("v1.3.1", DigestB), installs, changed);
        CHECK(!installs && changed);
        // The same tag with a replaced package is not the release that was accepted.
        windowSees(Release("v1.3.0", DigestB), installs, changed);
        CHECK(!installs && changed);
        // The release was withdrawn, or the window's check failed.
        windowSees(Release("v1.3.0", DigestA, false), installs, changed);
        CHECK(!installs && !changed);
        windowSees(Failure(), installs, changed);
        CHECK(!installs && !changed);
        // Another channel's release is not the one accepted, even with the same words.
        InstallOfferGate gate(InstallOffer{UpdateChannel::Stable, "v1.3.0", DigestA});
        CHECK(gate.Observe(UpdateChannel::Nightly, Release("v1.3.0", DigestA), true) == InstallOfferGate::Step::Changed);
        // A window started without an offer never installs on its own.
        InstallOfferGate none;
        CHECK(none.Observe(UpdateChannel::Stable, Release("v1.3.0", DigestA), true) == InstallOfferGate::Step::Wait);
    }

    std::cout << "Update policy tests passed.\n";
    return 0;
}
