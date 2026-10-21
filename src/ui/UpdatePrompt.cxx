#include "UpdatePrompt.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {

std::optional<launcher::InstallOffer> UpdatePrompt::Due(const platform::ServiceSnapshot& services, const Place& place) const {
    if (!services.launchOffer || open_ || !place.Open()) return std::nullopt;
    if (done_ && *done_ == *services.launchOffer) return std::nullopt;
    if (skip_ && skip_->offer == *services.launchOffer) return std::nullopt;
    return services.launchOffer;
}

NoticeDialog UpdatePrompt::Dialog(const launcher::InstallOffer& offer, const std::string& installed) {
    // Later is highlighted: a press meant for the menu behind must not close the game.
    // V2 names Updates beside the mark and the offer's channel at the top right.
    return NoticeDialog{loc::Tf("updates.launch_text", offer.tag, installed), loc::T("updates.launch_heading"),
        {{loc::T("updates.launch_now"), NoticeResult::UpdateNow}, {loc::T("updates.launch_later")},
         {loc::T("updates.launch_skip"), NoticeResult::SkipUpdate}}, 1,
        loc::T("updates.title"), {}, {loc::T(updates::GetUpdateChannelInfo(offer.channel).labelKey), v2::Colors().accent}};
}

std::optional<launcher::InstallOffer> UpdatePrompt::Close() {
    auto offer = std::move(open_);
    open_.reset();
    return offer;
}

std::optional<std::string> UpdatePrompt::Resolve(const platform::ServiceSnapshot& services) {
    if (!skip_) return std::nullopt;
    if (services.skipFailures > skip_->failuresSeen) {
        skip_.reset();
        return services.skipError.empty() ? loc::T("common.save_failed") : services.skipError;
    }
    if (!services.launchOffer || *services.launchOffer != skip_->offer) { done_ = skip_->offer; skip_.reset(); }
    return std::nullopt;
}

UpdatePrompt::Answer UpdatePrompt::AnswerOf(NoticeResult result) {
    return result == NoticeResult::UpdateNow ? Answer::UpdateNow : result == NoticeResult::SkipUpdate ? Answer::Skip : Answer::Later;
}

} }
