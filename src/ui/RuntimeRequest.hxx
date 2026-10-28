#pragma once
// What the menu asks of the netplay runtime, with no ImGui or game type in it,
// so the runtime's own command can be built on the same fields instead of a
// copy of each one (sf4e::NetplayFacade::RuntimeCommand).
#include "../common/ControllerRequest.hxx"
#include "../common/ReplayRequest.hxx"
#include "../common/RoomRules.hxx"
#include "../common/SteamId.hxx"
#include "../common/TrainingEntry.hxx"
#include "../common/UpdateChannel.hxx"
#include "../discord/Presence.hxx"
#include "../launcher/update/InstallOffer.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/InputAssignment.hxx"
#include "../netplay/MatchEndRules.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/SessionController.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../platform/ProblemReport.hxx"
#include "../session/RoomModel.hxx"
#include <cstdint>
#include <optional>
#include <string>

namespace sf4e { namespace ui {

struct RuntimeRequest {
    netplay::Command command{netplay::CommandKind::HostRoom};
    platform::ServiceAction service = platform::ServiceAction::None;
    // With ShowReplayFile: the archived replay, as its row names it, whose folder opens.
    std::string servicePath;
    // With UpdateNow or SkipUpdate: the launch offer the player was shown.
    launcher::InstallOffer updateOffer;
    // With SwitchUpdateChannel: the channel the player confirmed.
    updates::UpdateChannel updateChannel = updates::UpdateChannel::Stable;
    // With OpenSteamProfile: the member's profile to open.
    SteamId steamProfile;
    reports::Submission reportSubmission;
    input::Action inputAction = input::Action::None;
    gamepads::ControllerRequest padAction;
    discord::InviteAction discordAction = discord::InviteAction::None;
    std::uint64_t discordRevision = 0;
    netplay::PlayerPreferences preferences;
    room::Action roomAction;
    int selectedDelay=-1;
    // Plays the challenger call-out once at this volume (percent); -1 plays nothing.
    int previewSoundVolume=-1;
    // Asks for the room's short link; nothing else is sent.
    bool shortInvitation=false;
    // Saves only the settings a room leaves open, button layouts and the room
    // board's column, from `preferences` (PlayerPreferences::CopyRoomSettings); sent alone.
    bool roomSettings=false;
    // Leave a joined room's menu for Training mode (common/TrainingEntry.hxx);
    // sent alone. The runtime decides whether it still holds.
    TrainingEntry training=TrainingEntry::None;
    // Leave the game this PC watches (ShellView::watchedGame), named as the
    // press saw it; sent alone. The runtime decides, when it takes it, whether
    // the room still needs an Unwatch or only the rest of the stream ends.
    netplay::WatchedGame leaveWatchedGame;
    // An identity or bridge request; op None for everything else.
    netplay::IdentityRequest identity;
    // Refresh the assignment list, play a match or stop; op None for everything else.
    netplay::tournament::Command tournament;
    // With a JoinInvite: the signed ticket (JSON) of a public room's admission.
    std::string publicTicket;
    // With the JoinInvite of a public room just created: the table rules chosen
    // on Create, which the runtime sets once the creator is in it as host.
    std::optional<room::Rules> createdRules;
    // With HostRoom: the room is a Practice room for its life (room::Snapshot::practice).
    bool practiceRoom = false;
    // With HostRoom: whose input delay the room plays at for its life (room::Snapshot::delayPolicy).
    room::DelayPolicy delayPolicy = room::DelayPolicy::Shared;
    // What the Replays screen asks of the game: an archived replay (its
    // file, as its row names it) to add or to watch, the game's own list, or
    // no to a replay link. Nothing is sent to the room.
    replay::Request replay;
};

} }
