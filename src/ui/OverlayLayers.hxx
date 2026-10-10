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
    bool captionShown = false;
    ExportCaptionView caption;
    MatchStripView match;
    // A replay the game plays: its controls, lanes and frame meter.
    ReplayHudView replay;
};
TrainingHudInput DrawOverlayLayers(const OverlayLayersView& view, const training::View& training);
} }
