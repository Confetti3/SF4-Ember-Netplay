#pragma once
#include "Theme.hxx"
#include "TrainingPanel.hxx"
#include "ReplayPlaybackHud.hxx"

namespace sf4e { namespace ui {
// Presentation inputs only. The overlay owns hotkeys, commands and capture;
// this boundary owns which passive layers and alerts are drawn together.
struct OverlayLayersView {
    bool shellVisible = false, shellAvailable = false;
    bool trainingControlsOpen = false, nativePaused = false;
    bool focused = true, trainingHud = true, matchActive = false;
    bool matchWaitsForMenu = false, showMatchHud = true;
    std::string controllerWarning;
    // A replay is being exported. Its video's layers, the caption and the
    // frame meter, are then drawn whatever window is open over them, so the
    // video never depends on Ember's menu (SplitExportPasses).
    bool exporting = false;
    bool captionShown = false;
    ExportCaptionView caption;
    MatchStripView match;
    // A replay the game plays: its controls, lanes and frame meter.
    ReplayHudView replay;
    // A member waiting in Training: where they stand in their room, drawn
    // with the training HUD; empty outside a room.
    TrainingRoomStatus trainingRoom;
    // The call back from Training: who called, and go now's glyph.
    ChallengerCall challenger;
};
TrainingHudInput DrawOverlayLayers(const OverlayLayersView& view, const training::View& training);

// One overlay frame's drawing in two passes around an export's picture:
// video, the layers an export's video takes (the windows ExportCaptionWindow
// and FrameMeterWindow), and rest, all the rest of Ember: its menus, the
// passive HUD and their tooltips. Each keeps ImGui's order. The overlay draws
// video, the picture is taken, then rest; with nothing in video the picture
// is the game's alone.
struct ExportPasses { ImDrawData video, rest; };
void SplitExportPasses(const ImDrawData& all, ExportPasses& passes);
} }
