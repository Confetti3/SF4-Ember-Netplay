#include "sf4e__OverlayMatchStrip.hxx"

namespace sf4e { namespace overlay {
ui::MatchStripView MatchStripOf(const NetplayStatus& status, const netplay::PlayerPreferences& preferences) {
    ui::MatchStripView strip;
    for (int side = 0; side < 2; ++side) {
        strip.names[side] = status.matchSides[side].name; strip.links[side] = status.matchSides[side].link;
        strip.badges[side] = preferences.classEmblems ? status.matchSides[side].badge : SkillBadge();
    }
    if (status.hasMatchScore) strip.score = ui::SetScoreText(status.matchScore);
    strip.rollbackFrames = status.rollbackFrames;
    strip.pingMs = status.pingMs; strip.appliedDelay = status.appliedDelay;
    strip.spectator = status.spectator; strip.behindFrames = status.spectatorBacklog; strip.ownDelays = status.ownDelays;
    strip.queuePosition = status.queuePosition; strip.queueLength = status.queueLength;
    strip.size = preferences.matchHudSize; strip.raised = preferences.matchHudRaised; strip.anchor = preferences.matchHudAnchor;
    strip.layout = preferences.matchHudLayout; strip.nameOffset = preferences.matchHudNameOffset; strip.spectators = status.spectators;
    strip.notice = status.lastError; strip.noticeSeverity = static_cast<int>(status.lastErrorSeverity);
    strip.connectionWarning = status.connectionWarning; strip.predictionStalled = status.predictionStalled;
    strip.disconnectCountdownMs = status.disconnectCountdownMs;
    return strip;
}
} }
