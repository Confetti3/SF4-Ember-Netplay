#include "OverlayLayers.hxx"
#include "PassiveHint.hxx"
#include "OverlayPresentation.hxx"
#include "TrainingPanel.hxx"
#include "../common/Localization.hxx"
#include <cstring>

namespace sf4e { namespace ui {
TrainingHudInput DrawOverlayLayers(const OverlayLayersView& view, const training::View& training) {
    ChallengerBanner(training, view.challenger);
    bool passive = PassiveOverlayShown(view.shellVisible, view.trainingControlsOpen, view.nativePaused);
    TrainingHudInput hud;
    // Behind the HUD, and under the training controls too, so turning them on there shows them;
    // never over the game's pause menu, which the controls may be opened over.
    if (training.available && view.trainingHud && !view.nativePaused) DrawTrainingBoxes(training);
    if (passive && view.focused && training.available && view.trainingHud) {
        DrawTrainingRoomStatus(view.trainingRoom);
        // The Data panel goes where the HUD leaves room.
        hud = DrawTrainingHud(training, view.challenger.called);
        DrawTrainingData(training, hud.area);
        if (hud.open) passive = false;
    }
    // Never over a fight, where it would cover the timer; a Practice match's
    // own notice names the keys there.
    if (passive && view.shellAvailable && !view.matchActive) DrawShortcutHint(view.shortcutHintAlpha);
    if (passive && view.matchWaitsForMenu) DrawPassiveHint("Ember match waiting", loc::T("runtime.return_menu_to_join"), 44);
    if (passive && !view.matchActive && !view.padHint.empty()) DrawPassiveHint("Ember controller", view.padHint.c_str(), 76);
    if (view.captionShown) DrawExportCaption(view.caption);
    // A replay has no GGPO session, so its meter is drawn here, not with the
    // match's below. An export's stays drawn under a window, since it is in
    // the video; the strip and lanes are never drawn over an export.
    if (!view.matchActive && (passive || view.exporting) && view.replay.meter && training.watching) DrawMatchMeter(training);
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

void SplitExportPasses(const ImDrawData& all, ExportPasses& passes) {
    for (ImDrawData* pass : {&passes.video, &passes.rest}) {
        pass->Clear();
        pass->Valid = all.Valid; pass->DisplayPos = all.DisplayPos; pass->DisplaySize = all.DisplaySize;
        pass->FramebufferScale = all.FramebufferScale; pass->OwnerViewport = all.OwnerViewport;
    }
    for (ImDrawList* list : all.CmdLists) {
        const char* owner = list->_OwnerName ? list->_OwnerName : "";
        const bool video = !std::strcmp(owner, ExportCaptionWindow) || !std::strcmp(owner, FrameMeterWindow);
        (video ? passes.video : passes.rest).AddDrawList(list);
    }
}
} }
