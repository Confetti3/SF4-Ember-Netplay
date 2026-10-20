#include "OverlayLayers.hxx"
#include "OverlayPresentation.hxx"
#include "../common/Localization.hxx"
#include <cstring>

namespace sf4e { namespace ui {
TrainingHudInput DrawOverlayLayers(const OverlayLayersView& view, const training::View& training) {
    if (view.matchLoading) DrawMatchLoading();
    else ChallengerBanner(training, view.challenger);
    bool passive = !view.matchLoading && PassiveOverlayShown(view.shellVisible, view.trainingControlsOpen, view.nativePaused);
    TrainingHudInput hud;
    const bool trainingHud=training.available&&view.trainingHud;
    const bool matchHud=view.matchActive&&view.showMatchHud;
    if (passive && view.focused && trainingHud) {
        DrawTrainingRoomStatus(view.trainingRoom);
        hud = DrawTrainingHud(training, view.challenger.called);
        if (hud.open) passive = false;
    }
    const auto hint = [](const char* name, const char* text, float top, float alpha = 1) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        const auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + top * Scale()), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(text); ImGui::End();
        ImGui::PopStyleVar();
    };
    // Never over a fight, where it would cover the timer; a Practice match's
    // own notice names the keys there.
    if (passive && view.shellAvailable && !view.matchActive && view.shortcutHintAlpha > 0)
        hint("Ember shortcut", loc::T("runtime.open_shortcut"), 12, view.shortcutHintAlpha);
    if (passive && view.matchWaitsForMenu) hint("Ember match waiting", loc::T("runtime.return_menu_to_join"), 44);
    if (passive && !view.matchActive && !view.padHint.empty()) hint("Ember controller", view.padHint.c_str(), 76);
    if (!view.matchLoading && view.captionShown) DrawExportCaption(view.caption);
    // A replay has no GGPO session, so its meter is drawn here, not with the
    // match's below. An export's stays drawn under a window, since it is in
    // the video; the strip and lanes are never drawn over an export.
    if (!view.matchActive && (passive || (!view.matchLoading && view.exporting)) && view.replay.meter && training.watching) DrawMatchMeter(training);
    if (passive && view.replay.shown) {
        DrawReplayStrip(view.replay);
        if (view.replay.lanes) DrawReplayLanes(view.replay);
    }
    if (view.matchActive || view.matchLoading) {
        DrawControllerWarning(view.controllerWarning);
        if (passive && training.watching) DrawMatchMeter(training);
        const auto& match = view.match;
        if (passive && matchHud) DrawMatchStrip(match);
        else if (match.HasAlert() && (passive || match.NeedsAttention())) {
            // An open window suppresses telemetry, but a stalled or ending
            // fight still explains itself using the strip's own state line.
            const auto line = MatchStripStateLine(match);
            DrawMatchNotice(line, match.AlertSeverity());
        }
    }
    if (view.matchActive && passive && view.focused) {
        if (!view.match.spectator && view.practiceMatch)
            DrawMatchKeysNotice({"training.match.checkpoint_disabled", "training.match.end_hint"});
        else if (view.watchingHint) DrawMatchKeysNotice({"runtime.watching_hint"});
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
