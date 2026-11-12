#pragma once
// What the menu, and the runtime itself, ask of the netplay runtime: one type
// per command, each holding only what that command carries. The menu builds
// one where the player's choice is known (ui::ShellAction); the runtime takes
// it through one queue (NetplayFacade::SubmitRuntimeCommand) and handles each
// type on its own (sf4e__NetplayRuntime__Room.cxx). Nothing in it is ImGui's
// or the game's: the overlay, which holds the native pick, fills a Ready's
// pick, stage and Random stage pool as it hands the command on.
#include "../common/ControllerRequest.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/ReplayRequest.hxx"
#include "../common/StageCatalog.hxx"
#include "../common/SteamId.hxx"
#include "../common/TrainingEntry.hxx"
#include "../common/UpdateChannel.hxx"
#include "../discord/Presence.hxx"
#include "../launcher/update/InstallOffer.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../platform/ProblemReport.hxx"
#include "../session/RoomModel.hxx"
#include "FriendCommands.hxx"
#include "IdentityRequest.hxx"
#include "InputAssignment.hxx"
#include "MatchEndRules.hxx"
#include "PlayerPreferences.hxx"
#include "SessionController.hxx"
#include "TableSetup.hxx"
#include "TournamentStatus.hxx"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace sf4e { namespace commands {

// How the room being joined was entered: a private invitation, a public room's
// ticket (a refusal or failure is worded for it), or a tournament match's
// watcher (the room admits it only if it names it).
enum class JoinKind : std::uint8_t { Private, Public, Observer };

// An identity or bridge request. Endpoint-level: no room generation applies.
struct Identity { netplay::IdentityRequest identity; };
// Refresh the assignment list, play a match, stop, and the bridge's other asks.
struct Tournament { netplay::tournament::Command tournament; };
// What the friends screens ask is one type per request (netplay/FriendCommands.hxx,
// Friend*). Join, Watch, TakeInvite and AnswerVisit carry the generation the
// press saw and are dropped once it has moved on; the rest need none.

// Every command below is made for the controller generation the press saw
// (netplay::Snapshot::generation), and is dropped once that has moved on.

// Leave the menu for Training, from a room or Home (common/TrainingEntry.hxx).
// The runtime decides when it takes it whether that still holds.
struct Training { netplay::Generation generation; TrainingEntry training = TrainingEntry::Home; };
// Leave the game this PC watches (ShellView::watchedGame), named as the press
// saw it. The runtime decides, when it takes it, whether the room still needs
// an Unwatch or only the rest of the stream ends.
struct LeaveWatchedGame { netplay::Generation generation; netplay::WatchedGame leaveWatchedGame; };
// The player's answer to a pending Discord invitation, for the revision they saw.
struct DiscordInvite {
    netplay::Generation generation;
    discord::InviteAction discordAction = discord::InviteAction::Cancel;
    std::uint64_t discordRevision = 0;
};
// Plays the challenger call-out once at this volume (percent).
struct PreviewSound { netplay::Generation generation; int previewSoundVolume = 0; };
// What the Replays screen asks of the game: an archived replay (its file, as
// its row names it) to add or to watch, the game's own list, or no to a replay
// link. Nothing is sent to the room.
struct Replay { netplay::Generation generation; replay::Request replay; };
// Asks for the room's short link.
struct ShortInvitation { netplay::Generation generation; };
struct Pad { netplay::Generation generation; gamepads::ControllerRequest padAction; };
// Saves only the settings a room leaves open: button layouts and the room
// board's layout (column, split, panels, picture), from `preferences`
// (PlayerPreferences::CopyRoomSettings), whenever the room leaves them open
// (netplay::RoomSettingsOpen), watching a game included.
struct RoomSettings { netplay::Generation generation; netplay::PlayerPreferences preferences; };
struct Input { netplay::Generation generation; input::Action inputAction = input::Action::Cancel; };
struct Service {
    netplay::Generation generation;
    platform::ServiceAction service = platform::ServiceAction::None;
    // With ShowReplayFile: the archived replay, as its row names it, whose folder opens.
    std::string servicePath;
    // With UpdateNow or SkipUpdate: the launch offer the player was shown.
    launcher::InstallOffer updateOffer;
    // With SwitchUpdateChannel: the channel the player confirmed.
    updates::UpdateChannel updateChannel = updates::UpdateChannel::Stable;
    // With OpenSteamProfile: the member's profile to open.
    SteamId steamProfile;
    // With SendReport.
    reports::Submission reportSubmission;
};

// The room commands, which the session controller decides (netplay::CommandKind).

// Opens a private room with this PC as its host.
struct HostRoom {
    netplay::Generation generation;
    // Saved as the player's own as the room opens (netplay::AcceptedPreferences).
    netplay::PlayerPreferences preferences;
    // The room's type, Practice, delay policy, table count and Wired players only rule.
    room::Charter charter;
    // Opened from a template: each table's rules and name, which the runtime
    // sets once the room is up (netplay::CreatedRules).
    std::optional<netplay::TableSetup> createdRules;
};
struct JoinRoom {
    netplay::Generation generation;
    std::string invitation;
    // A public room's admission: the bridge's signed ticket (JSON). Empty for a private room.
    std::string publicTicket;
    // Private, or Observer for a tournament match's watcher, which the room
    // admits only when it names it. Public is never set here: a public room's
    // join is the one with a publicTicket.
    JoinKind join = JoinKind::Private;
    // From a Discord invitation: checked again when it runs (discord::PendingInvite).
    std::uint64_t discordRevision = 0;
    // The join of a public room just created: each table's rules and name
    // chosen on Create, which the runtime sets once the creator is in it as
    // host, with the type, table count and Wired players only rule of
    // `charter` (netplay::CreatedRules). Nothing for any other room.
    std::optional<netplay::TableSetup> createdRules;
    room::Charter charter;
};
struct LeaveRoom {
    netplay::Generation generation;
    // From a Discord invitation, whose join follows: checked again when it runs.
    std::uint64_t discordRevision = 0;
};
// The replacement room the room offers when its host is gone.
struct ReplaceRoom { netplay::Generation generation; };
struct StartOffline { netplay::Generation generation; };
// Saves every setting; only outside a room (ShellView::canEditPreferences).
struct SavePreferences { netplay::Generation generation; netplay::PlayerPreferences preferences; };
// Player 1's lobby rules for the next match.
struct LobbySettings { netplay::Generation generation; netplay::LobbySettings settings; };
// Ready at this PC's seat, or a Rematch over the WIN (which may also go on in
// place). The menu sends it bare; the overlay adds the pick, the stage and the
// stages Random may land on (P1 resolves Random with them).
struct Ready {
    netplay::Generation generation;
    bool rematch = false;
    selection::Pick pick;
    int stage = 0;
    selection::RandomPool randomPool;
};
// Any room action (Queue, Watch, chat, the host's edits); the room authority decides it.
struct RoomAction { netplay::Generation generation; room::Action roomAction; };
struct CheckConnection { netplay::Generation generation; bool benchmark = false; };
// The input delay to play at, Auto (AutoInputDelayChoice), or -1 for the
// connection check's recommendation.
struct ApplyDelay { netplay::Generation generation; int selectedDelay = -1; };

} // namespace commands

// One command for the runtime, and the same with other alternatives after it
// (ui::ShellAction adds the overlay's own steps).
template <class... Extra>
using RuntimeRequestWith = std::variant<commands::Identity, commands::Tournament,
    commands::FriendsVisit, commands::FriendsLeave, commands::FriendRequest, commands::FriendRequestByCode, commands::FriendAccept,
    commands::FriendDecline, commands::FriendCancel, commands::FriendRemove, commands::FriendBlock, commands::FriendUnblock,
    commands::FriendNewCode, commands::FriendAppearOffline, commands::FriendTurnOff, commands::FriendTurnOn, commands::FriendInvite,
    commands::FriendDismiss, commands::FriendJoin, commands::FriendWatch, commands::FriendTakeInvite, commands::FriendAnswerVisit,
    commands::Training, commands::LeaveWatchedGame,
    commands::DiscordInvite, commands::PreviewSound, commands::Replay, commands::ShortInvitation, commands::Pad,
    commands::RoomSettings, commands::Input, commands::Service,
    commands::HostRoom, commands::JoinRoom, commands::LeaveRoom, commands::ReplaceRoom, commands::StartOffline,
    commands::SavePreferences, commands::LobbySettings, commands::Ready, commands::RoomAction,
    commands::CheckConnection, commands::ApplyDelay, Extra...>;
using RuntimeRequest = RuntimeRequestWith<>;

} // namespace sf4e
