#pragma once
// What a drawn frame of the shell reads: one copied view of the game, the
// runtime and the player, built by the caller (sf4e__Overlay.cxx, the lab).
#include <imgui.h>
#include "RoomCards.hxx"
#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplayRequest.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/TrainingEntry.hxx"
#include "../common/GameDisplayConfig.hxx"
#include "../discord/Presence.hxx"
#include "../netplay/InputAssignment.hxx"
#include "../netplay/TableSetup.hxx"
#include "../netplay/SessionController.hxx"
#include "../netplay/MatchEndRules.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../platform/Speech.hxx"
#include "../netplay/MemberView.hxx"
#include "../netplay/IdentityView.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/Answered.hxx"
#include "../netplay/Archive.hxx"
#include "../netplay/FriendsView.hxx"
#include "../netplay/Levels.hxx"
#include "../netplay/ProfileRequests.hxx"
#include "../netplay/Profiles.hxx"
#include "../netplay/Rankings.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../session/RoomFollow.hxx"
#include "../session/RoomModel.hxx"
#include "../session/MemberLatency.hxx"
#include "UpdateChannelPick.hxx"
#include "RoomBoardArea.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include "GameMenu.hxx"
#include "PostMatchPrompt.hxx"
#include "../common/ControllerView.hxx"
#include "../common/ControllerRequest.hxx"
#include "../common/ButtonLayoutCapture.hxx"
#include "ChatTranscript.hxx"
#include "ReplaysView.hxx"
#include "V2.hxx"

namespace sf4e { namespace ui {

// The shared shell has no game, transport, filesystem or bootstrap dependency.
// Its caller supplies a copied view and translates actions on the owning thread.
struct ShellView {
    bool controllerAvailable = false, controllerFocus = false, controllerBack = false;
    bool controllerUnavailable = false;
    netplay::Snapshot session;
    room::Snapshot room;
    // How long ago `room` was stamped, in ms (RuntimeSnapshot::roomReceivedMs to
    // now; 0 without a stamp). Idle times and start holds count on from the
    // stamp where they are read (RoomIdleSeconds, RoomRemainingMs), so `room`
    // is never patched and a frame can be lent the same copy as the last (RoomCarry).
    std::uint64_t roomElapsedMs = 0;
    // The room events the chat shows that no snapshot carries (ChatTranscript).
    std::vector<RoomNotice> roomNotices;
    netplay::PlayerPreferences preferences;
    // The title and icon the player wears on their card. The identity's
    // backend fills them once it awards titles; none until then.
    std::string playerTitle;
    v2::Tier playerTier = v2::Tier::None;
    int playerIcon = -1;
    netplay::LobbySettings lobbySettings;
    bool helperReady = false, canOpenRoom = false, canReady = false;
    bool canReplaceRoom = false;
    // Launcher --host-public-room: keep a public room open (UpdateHostedPublicRoom).
    bool hostPublicRoom = false;
    // canChangeUltra: canEditSelection, or over the WIN until the post-match
    // prompt is answered (RuntimeGates::canChangeUltra).
    bool canEditSelection = false, canChangeUltra = false;
    bool canEditPreferences = false, canEditLobby = false, settingsPending = false;
    // The settings a room leaves open (PlayerPreferences::CopyRoomSettings: the
    // room board's layout and the watched game's picture) can change now: with
    // every preference open, or while the room leaves them open
    // (netplay::RoomSettingsOpen, watching a game included), where the shell
    // saves them alone (UpdatePreferenceSave) and the runtime takes them at once
    // on the same rule (SaveRoomSettings). Every row, grip and picture for those
    // fields asks this; button layouts ask CanEditLayout, at the main menu only.
    bool CanEditRoomSettings() const {
        return canEditPreferences || netplay::RoomSettingsOpen(session.room, session.match);
    }
    int selectedDelay=2, recommendedDelay=-1, opponentDelay=-1;
    bool autoDelayMeasured=false;
    bool delayLocked=false, canProbe=false, canApplyDelay=false;
    std::string probeStatus;
    RouteKind probeRoute=RouteKind::Unknown;
    // Where a relayed route goes (a region code) and who is on the other side,
    // for the connection check: the opponent's name and how their network
    // reported it. netReport is this PC's own.
    std::string probeRelay, probeOpponent;
    NatClass probeOpponentNat=NatClass::Unknown;
    NetworkSummary netReport;
    std::uint64_t probeP50Us=0, probeP95Us=0, probeP99Us=0, probeJitterUs=0;
    bool probeBenchmark=false;
    unsigned probeSamples=0, probeLost=0, probeSent=0, probeExpected=0;
    // The ping to each other member while the room board is open, by member.
    session::MemberLatencyTable memberLatency;
    int localSlot = -1;
    std::string invitation, error, settingsError, build;
    // The room's short link, empty until the helper has one. A failure bumps
    // the counter; Copy short link then copies the full invitation.
    std::string shortInvitation;
    bool shortInvitationPending = false;
    std::uint64_t shortInvitationFailures = 0;
    // The newest room link opened from the browser, and its sequence.
    std::string pendingJoinLink;
    std::uint64_t pendingJoinSequence = 0;
    // That link arrived while the player was free and is still fresh
    // enough to join by itself.
    bool pendingJoinDirect = false;
    std::string languagePreference = "auto";
    // The Settings row "Check for updates at launch", as saved.
    bool checkUpdatesAtLaunch = true;
    // The country Automatic shows: Windows' region, or none.
    CountryCode automaticCountry;
    // This PC's clock, unix seconds, to tell a match past its expiry (0 when unknown).
    std::uint64_t unixNow = 0;
    // The game's own config.ini as read at launch, and whether the player has
    // already dismissed the card for good.
    gameconfig::DisplaySettings gameSettings;
    bool showGameSettingsCard = false;
    std::vector<netplay::MemberView> members;
    netplay::NetworkAvailability network = netplay::NetworkAvailability::Starting;
    platform::ServiceSnapshot services;
    std::string controller;
    std::string padHint;
    std::uint64_t padHintUntil = 0;
    gamepads::Layer padLayer = gamepads::Layer::Active;
    bool padLayerPending = false, runningControllerLayer = true;
    // The game's menu music entry is the supported one, so Endless lobby music can be turned on.
    bool lobbyMusicAvailable = false;
    // The themes in <game>\ember\themes with each part (netplay::GameThemePart), by name.
    std::array<std::vector<std::string>, netplay::GameThemePartCount> gameThemes;
    std::vector<gamepads::PadView> pads;
    unsigned captureCandidate = 0;
    gamepads::SetupSnapshot padSetup;
    bool atMainMenu = true;
    // gameplayBrand is detected; promptBrand is resolved by the runtime.
    // The local unsaved setting can resolve against gameplayBrand immediately.
    gamepads::PromptBrand gameplayBrand, promptBrand;
    bool discordPending = false, discordConfirm = false, discordCanSwitch = false;
    std::uint64_t discordRevision = 0;
    std::string discordStatus;
            input::Capture inputCapture = input::Capture::Idle;
            input::Device inputDevice;
    // The gameplay controller's menu buttons and glyphs (Pad::MenuButtons).
    input::MenuButtons menuButtons;
            bool canChangeController = false, controllerReady = false;
    std::string selectionSummary, selectionError;
    // The Training dummy's pick, as selectionSummary reads (Training select).
    std::string dummySummary,dummyError;
    // The chosen fighter, Ultra, appearance, fighter options and stage, for
    // the table page's rows; ultraSteps and colorSteps when there is more than
    // one to step through.
    std::string fighterName, ultraName, appearanceName, fighterOptionsName, stageName;
    bool ultraSteps = false, colorSteps = false;
    std::string selectionLockReason;
    // A Ready press in flight, and the last failure with a sequence that
    // changes per occurrence.
    netplay::ReadyPress readyPress = netplay::ReadyPress::None;
    std::string readyFailure;
    std::uint64_t readyFailureSequence = 0;
    // The fighter the opponent changed to since the player last readied
    // (-1: none), and a sequence that changes per change.
    int opponentChangedFighter = -1;
    std::uint64_t opponentChangeSequence = 0;
    // Called out of Training to the player's table (RuntimeSnapshot's fields
    // of the same names): show the table, ready the player, the seconds left
    // to ready, and whether Training can be entered from the room.
    std::uint64_t trainingCallSequence = 0, trainingReadySequence = 0;
    int trainingReadySeconds = 0;
    bool canTrain = false;
    // This PC's own pick, so its seat on the room board shows the fighter,
    // costume, colour and Ultra at once, before Ready shares them.
    selection::Pick selectedPick;
    // The pick is Random: selectedPick is the fighter it rolled (docs/design/RANDOM_FIGHTER.md).
    bool selectedRandom = false;
    // This PC fights in a Practice match now (input::MenuContext::PracticeMatch):
    // Ember's menu is open over the fight, and End practice ends the game.
    bool practiceFight = false;
    // The room game this PC only watches now (RuntimeSnapshot::watchedGame,
    // input::MenuContext::WatchedMatch): Ember's menu is open over it, and Stop
    // watching leaves it. Empty when nothing is watched.
    netplay::WatchedGame watchedGame;
    // The WIN screen of a room game this PC fought, at a table with Instant
    // rematch on (input::MenuContext::MatchResult): the post-match prompt opens.
    bool matchResult = false;
    // Over that WIN, the seats whose player chose Rematch (bit 1 = P1, 2 = P2),
    // and the games of the battle that went on to a rematch in place.
    unsigned rematchChoices = 0, gamesInPlace = 0;
    // The room game this PC fights: its table and generation, kept when an
    // Arcade room moves the set's loser on, and the WIN's result.
    netplay::FoughtGame foughtGame;
    // That game's picture, for the menu's top right corner over an opaque
    // menu (PlayerPreferences::pictureMode); null when it is not wanted or
    // could not be taken.
    ImTextureID watchedPicture{};
    // Ember's menu shows the watched game through itself, dimmed, instead.
    bool SeeThrough() const { return watchedGame.Active() && !watchedPicture; }
    // Lock-ins lost to a failure of this member's spectator view
    // (RuntimeSnapshot::lockInDrops); each new count is told once.
    std::uint64_t lockInDrops = 0;
    // The Ember identity (RuntimeSnapshot::identity and its request fields).
    netplay::IdentityView identity;
    std::uint64_t identityTicket = 0, identityRequest = 0;
    std::string identityRefusal;
    // The tournament match being played and the assignment list (RuntimeSnapshot::tournament).
    netplay::tournament::Status tournament;
    // The bridge's public rooms and the answer to the last create or ticket request.
    netplay::publicrooms::Status publicRooms;
    netplay::rankings::Status rankings;
    // The public replay archive's listing as the Replays screen's Archive tab asked for it, and the
    // download of a game to watch.
    netplay::archive::Status archiveBrowse;
    // The profile page's data (docs/design/PROFILE.md). Commands: commands::ProfileOpen, ProfileClose and PlayerSearch.
    // The profile open: how it was opened, what the opener knew, and the card as it arrives. `state` Closed when none
    // is open; see netplay::profiles::Profile for each state's meaning and what each holds.
    netplay::profiles::Profile profile;
    // Find a player's search as the player last typed it, and its answer. Empty `text` when there is none.
    netplay::profiles::SearchView playerSearch;
    // The players this PC fought in public rooms lately, newest first, none past 7 days (at most
    // netplay::profiles::MaxRecent); empty when there are none. Kept on this PC, never sent.
    std::vector<netplay::profiles::Recent> recentOpponents;
    // The sets of the profile open: a second slot of the Archive tab's browse state, so a profile's sets never replace
    // the Archive tab's. Asked for through the same Command ops as the archive's, with `archiveProfile` set and the
    // filter's `name` the player's; the service's `matching` count is not kept when it listed other players' sets
    // (netplay::archive::KeepNamed). Idle until a first page is asked for.
    netplay::archive::Status profileSets;
    // Whether the service offers profiles, as far as answers have said: Unknown until one does. Missing: no search by
    // name, no name, request or code openers (Find a player is by code, as 1.6.0); cards open by token and friend id
    // with 1.6.0's fields.
    netplay::profiles::Service profilesService = netplay::profiles::Service::Unknown;
    // How this PC reaches the internet as the card save last measured it (read at most once a minute, with a service and an
    // Ember ID in use); Unknown until then. The own card's Connection chip.
    NetworkLink ownLink = NetworkLink::Unknown;
    // The player's own fighter levels, as last read.
    netplay::Answered<netplay::levels::Own> levels;
    // The cards of a public room's members by card token, and of the friends the Friends tab focused lately by friend id
    // ("fr_..."; commands::FriendCardWanted); null while there are none.
    std::shared_ptr<const netplay::levels::CardMap> memberCards;
    // The player's friends, the action in flight and the last refusal's words
    // (netplay/FriendsView.hxx); commands go as the commands::Friend* types.
    netplay::friends::View friends;
    // Ember's replay archive, newest first, and what the runtime says of
    // replays (RuntimeSnapshot::Replays, ui/ReplaysView.hxx), as published.
    // A replay link's file waits there until the player answers: the Replays
    // screen opens with the question as its first row.
    ReplaysView replays;
};
} }
