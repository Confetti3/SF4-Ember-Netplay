#include "UpdatePolicy.hxx"

namespace sf4e { namespace platform {

UpdatePolicy::UpdatePolicy() {
    store_.load = [] { return LoadUpdatePreferences(); };
    store_.saveLastCheck = [](std::uint64_t time) { std::string error; return SaveLastUpdateCheck(time, error); };
    store_.saveSkipped = [](const std::string& tag) { std::string error; return SaveSkippedUpdate(tag, error); };
}

bool UpdatePolicy::ShouldCheck(std::uint64_t now) const {
    const auto saved = store_.load();
    if (!saved.checkAtLaunch) return false;
    return !saved.lastCheck || now < saved.lastCheck || now - saved.lastCheck >= CheckCooldownSeconds;
}

void UpdatePolicy::CheckFinished(bool reached, std::uint64_t now) {
    if (reached) store_.saveLastCheck(now);
}

std::optional<launcher::InstallOffer> UpdatePolicy::Present(launcher::UpdateChannel channel, const launcher::UpdateCheckResult& result) const {
    // The launch question never asks to go back.
    auto offer = result.goesBack ? std::nullopt : launcher::OfferFrom(channel, result);
    if (offer && offer->tag == store_.load().skipped) return std::nullopt;
    return offer;
}

bool UpdatePolicy::Skip(const std::string& tag) {
    return store_.saveSkipped(tag);
}

} }
