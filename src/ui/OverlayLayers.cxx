#include "OverlayLayers.hxx"
#include "OverlayPresentation.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
TrainingHudInput DrawOverlayLayers(const OverlayLayersView& view, const training::View& training) {
    ChallengerBanner(training, view.challenger);
    bool passive = PassiveOverlayShown(view.shellVisible, view.trainingControlsOpen, view.nativePaused);
    TrainingHudInput hud;
    if (passive && view.focused && training.available && view.trainingHud) {
        DrawTrainingRoomStatus(view.trainingRoom);
        hud = DrawTrainingHud(training);
        if (hud.open) passive = false;
    }
    const auto hint = [](const char* name, const char* key, float top) {
        const auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + top * Scale()), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(loc::T(key)); ImGui::End();
    };
    if (passive && view.shellAvailable) hint("Ember shortcut", "runtime.open_shortcut", 12);
    if (passive && view.matchWaitsForMenu) hint("Ember match waiting", "runtime.return_menu_to_join", 44);
    if (view.captionShown) DrawExportCaption(view.caption);
    // A replay has no GGPO session, so its meter is drawn here, not with the
    // match's below; the strip and lanes stay out of an export's video.
    if (!view.matchActive && passive && view.replay.meter && training.watching) DrawMatchMeter(training);
    if (passive && view.replay.shown) {
        DrawReplayStrip(view.replay);
        if (view.replay.lanes) DrawReplayLanes(view.replay);
    }
    if (view.matchActive) {
        DrawControllerWarning(view.controllerWarning);
        if (passive && training.watching) DrawMatchMeter(training);
        const auto& match = view.match;
        if (passive && view.showMatchHud) DrawMatchStrip(match);
        else if (passive || (match.noticeSeverity >= 2 && !match.notice.empty()) ||
            match.predictionStalled || (match.connectionWarning && match.disconnectCountdownMs >= 0)) {
            // An open window suppresses telemetry, but a stalled or ending
            // fight still explains itself using the strip's own state line.
            const auto line = MatchStripStateLine(match);
            const int severity = match.noticeSeverity >= 2 ? 2 :
                (match.connectionWarning || match.predictionStalled) ? 1 : match.noticeSeverity;
            DrawMatchNotice(line, severity);
        }
    }
    return hud;
}
} }
