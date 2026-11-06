#pragma once
#include <string>
#include <map>
#include <vector>
#include "../common/ButtonPrompts.hxx"
#include "../common/ButtonLayoutTypes.hxx"
#include "../common/Country.hxx"
#include "../common/GameCatalog.hxx"
#include "../common/InputDelay.hxx"
#include "../common/RoomRules.hxx"
#include "ProfileRecord.hxx"
#include "SpeechPreferences.hxx"
#include "PublicRoomFilter.hxx"
#include "TableSetup.hxx"

namespace sf4e { namespace netplay {

struct LobbySettings {
    bool editionSelect = true;
    int roundCount = 3;
    int roundTime = 99;
    bool operator==(const LobbySettings& other) const {
        return editionSelect == other.editionSelect && roundCount == other.roundCount && roundTime == other.roundTime;
    }
    bool Valid() const {
        return room::ValidRoundCount(roundCount) && room::ValidRoundTime(roundTime);
    }
};

// How far the name plates move either way, in 720p game units.
constexpr int MaxMatchHudNameOffset = 60;
// The match HUD positions run from 0 (bottom center) to this one (top center).
constexpr int MaxMatchHudAnchor = 5;
// The room board's layout grips (Customise.dc.html), in percent: the members
// and chat column's share of the board's width, and the members' share of that
// column over chat. Each snaps to its step from the default.
constexpr int PictureSmall = 0, PictureBig = 1, PictureOff = 2;
constexpr int MinRoomColumn = 28, MaxRoomColumn = 60, DefaultRoomColumn = 36, RoomColumnStep = 4;
constexpr int MinRoomSplit = 26, MaxRoomSplit = 74, DefaultRoomSplit = 56, RoomSplitStep = 6;

// A room saved to open again (Create › Template, Room settings › Save as
// template): the room's own settings and each table's rules and name.
struct RoomTemplate {
    std::string name;
    std::string roomName = "Private room";
    int capacity = 16;
    bool isPublic = false, practice = false;
    room::DelayPolicy delayPolicy = room::DelayPolicy::Own;
    // How many tables a private Classic room opens with (room::Snapshot::tableCount).
    int tableCount = static_cast<int>(room::ClassicTableCount);
    // The room's Wired players only rule (room::Snapshot::wiredOnly).
    bool wiredOnly = false;
    TableSetup tables;
    bool Valid() const {
        if (name.empty() || name.size() > 32 || roomName.empty() || roomName.size() > 64 || capacity < 2 || capacity > static_cast<int>(room::MaxMembers) ||
            (isPublic && practice) || tableCount < 1 || tableCount > static_cast<int>(room::TableCount) || !tables.Valid()) return false;
        for (unsigned char c : name + roomName) if (c < 32 || c == 127) return false;
        return true;
    }
    bool operator==(const RoomTemplate& rhs) const {
        return name == rhs.name && roomName == rhs.roomName && capacity == rhs.capacity && isPublic == rhs.isPublic &&
            practice == rhs.practice && delayPolicy == rhs.delayPolicy && tableCount == rhs.tableCount && wiredOnly == rhs.wiredOnly &&
            tables == rhs.tables;
    }
};
constexpr std::size_t MaxRoomTemplates = 12;

struct PlayerPreferences {
    std::string displayName = "Player";
    int mainFighter = 0;
    ProfileRecord record;
    int inputDelay = DefaultInputDelay;
    // Auto readies with the connection check's delay (AutoInputDelay) instead of
    // inputDelay. It is on until the player chooses a number.
    bool autoInputDelay = true;
    // How Auto uses the recommendation (AutoDelayStyle), Balanced unless chosen.
    int autoDelayStyle = 0;
    // On by default so a stall or rollback spike is visible, but Small: the standard strip drew the eye mid-fight.
    bool showMatchHud = true;
    int matchHudSize = 0;
    bool matchHudRaised = false;
    // "F10 or Start" for a few seconds each time Ember's menu closes.
    bool showShortcutHint = true;
    // Where the strip sits: 0 bottom center, 1 bottom left, 2 bottom right, 3 top left, 4 top right,
    // 5 top center (above the game's round timer).
    int matchHudAnchor = 0;
    // Where the watched game's picture sits: its corner (0 top right, 1 top
    // left, 2 bottom right, 3 bottom left) plus 4 times its size (0 small,
    // 1 medium, 2 large). Set from Settings or by dragging the picture.
    int pictureSpot = 0;
    // The award (v2::Awards' id) whose title, and whose icon, the player wears;
    // empty for the identity's own. Kept here until the backend stores it.
    std::string wornTitle, wornIcon;
    // The room board's column and the members' share of it (MinRoomColumn).
    int roomColumn = DefaultRoomColumn, roomSplit = DefaultRoomSplit;
    // 0 the Ember strip (one panel), 1 split: names over the game's PLAYER labels and a small telemetry panel.
    int matchHudLayout = 1;
    // Moves the split layout's name plates down (positive) or up, in 720p game units.
    // USF4's own HUD position option moves the PLAYER labels the plates cover.
    int matchHudNameOffset = 0;
    // The announcer calls out when the other fighter at this player's table readies.
    bool readySound = true;
    // Public room classes show beside players' names on this screen. Off hides
    // them here only: games still count and others still see this player's.
    bool classEmblems = true;
    // TV mode (Settings > Interface): a quarter larger, inside a 5% safe area.
    bool tvMode = false;
    // The first-run welcome (speech, language, controller) was shown or
    // skipped. A profile saved before it existed counts as welcomed; a fresh
    // install is set false when the runtime loads it (StartHelper).
    bool welcomed = true;
    // Called out of Training to their table, the player is readied at once
    // instead of being given the time to ready themselves.
    bool trainingAutoReady = false;
    // The game the player watches, over Ember's menu: PictureSmall in a corner
    // (pictureSpot) over an opaque menu, PictureBig in the room board's
    // tables' place (a corner elsewhere), PictureOff dimmed behind the menu.
    int pictureMode = PictureSmall;
    // The room board's Members and Chat panels; hidden, the other one or
    // the tables take their place.
    bool roomShowMembers = true, roomShowChat = true;
    // The room board's tables: one per line, each player's fighter at their
    // end of it, or with roomTableGrid two or three across.
    bool roomTableGrid = false;
    // The room board's fighters: each player's whole figure in their costume
    // and colour, or with roomCharacterPictures off the game's portraits.
    bool roomCharacterPictures = true;
    // Room chat is shown with listed words hidden (ui::FilterChat). Only this
    // player's screen changes; the room still holds what was written.
    bool chatFilter = true;
    // The training frame meter over the player's own matches and those they
    // watch. It only reads the game and shows confirmed frames.
    bool matchFrameMeter = false;
    // Percent of the game's own voice volume, in steps of ten.
    int readySoundVolume = 100;
    // The game's sound and the pads keep working while another window, such
    // as OBS, is in front (sf4e__BackgroundPlay.cxx).
    bool backgroundPlay = false;
    // The main menu plays a stage or character theme, another one after every
    // match, instead of its own track (sf4e__MenuMusic.cxx).
    bool endlessLobbyMusic = true;
    // Experimental (sf4e__InputLag.cxx): the battle reads each input in the
    // frame it was made instead of the next. A room's match does so only when
    // both fighters have it on, for them and whoever watches; offline battles
    // follow this setting.
    bool lowerInputLag = false;
    // Experimental: key presses become key states as the pad update reads
    // them, not a frame later. Only this PC's keyboard changes.
    bool lowerKeyboardLag = false;
    // The game draws each frame on its own thread, a frame behind; on, Ember
    // starts the game with that thread off (GameDisplayConfig.hxx:
    // RenderingThreadOverride). Read at startup, so a change takes effect on
    // the next launch. It changes only how this PC draws, never the match.
    bool fasterDisplay = true;
    // The game renders at its monitor's largest 16:9 size, in a window
    // without a frame, with black bars on a monitor of another shape
    // (sf4e__Borderless.hxx). Read at startup, like fasterDisplay. Only this
    // PC's picture changes.
    bool borderless = false;
    gamepads::ButtonPrompts buttonPrompts = gamepads::ButtonPrompts::Automatic;
    bool controllerLayer = true;
    std::map<std::string, std::string> controllerMappings;
    std::vector<gamepads::StoredLayout> buttonLayouts;
    // A match this PC only watches is recorded too, like one it plays
    // (sf4e__UserApp.cxx: StartMatchFromLobby).
    bool recordWatched = true;
    // Export video's caption parts and Frame meter as the player last set them,
    // so each export starts from their choice (ReplaysPanel::OpenExport).
    // Player names and Set score still start off for a replay whose names or
    // set Ember does not know.
    bool exportNames = true, exportLine = true, exportSet = true, exportMark = true, exportMeter = false;
    // Games played outside a tournament go to the public replay archive
    // (docs/design/REPLAY_ARCHIVE.md). A tournament set's games always do.
    bool shareReplays = true;
    // When sharing came on (unix seconds), 0 while it is off: only games saved
    // since go to the archive. Set only where preferences are loaded and
    // accepted (ShareReplaysSince), never by the sending (SendSetReplays).
    std::uint64_t shareReplaysSince = 0;
    // Room members can open this player's Steam profile from the room's member
    // screen. Sent when they join a room; nothing is sent when it is off.
    bool shareSteamProfile = true;
    // The flag beside this player's name in rooms and on the player card:
    // Windows' region (Automatic), a country they picked, or Hidden. Sent when
    // they join a room; nothing is sent while it is Hidden or Windows names none.
    CountryPreference country;
    bool discordPresence = true, discordInvites = true;
    // The launcher sends a crash's report without asking, with the small dump
    // and at most three a day (platform/ReportWorkflow.hxx). Off until the
    // player turns it on in Settings, Problem reports, or chooses Always send
    // after a crash. Saved as sendProblemReportsWithDump: the setting saved
    // before reports carried the dump, under sendProblemReports, is not read,
    // so every player is asked again.
    bool sendProblemReports = false;
    float interfaceScale = 1.f;
    // Takes `from`'s settings a room leaves open, as they change nothing about
    // play: the button layouts, the room board's layout, the chat filter and
    // the watched game's picture, which the board's grips, Hide and Show, More… and
    // Settings › Interface › Room layout change in a room
    // (ShellAction::roomSettings). The shell sends these alone while a room
    // locks the rest (OnlyRoomSettingsChanged), and the runtime saves these
    // alone then (SaveRoomSettings), both while netplay::RoomSettingsOpen holds.
    void CopyRoomSettings(const PlayerPreferences& from) {
        buttonLayouts = from.buttonLayouts;
        roomColumn = from.roomColumn; roomSplit = from.roomSplit;
        roomShowMembers = from.roomShowMembers; roomShowChat = from.roomShowChat; roomTableGrid = from.roomTableGrid;
        roomCharacterPictures = from.roomCharacterPictures; chatFilter = from.chatFilter;
        pictureMode = from.pictureMode; pictureSpot = from.pictureSpot;
    }
    SpeechPreferences speech;
    // Which rooms the Public rooms list shows (Region, Type, Open seats,
    // Connection); the default shows every room.
    publicrooms::RoomFilter roomFilter;
    LobbySettings lobby;
    std::string roomName = "Private room";
    int roomCapacity = 16;
    // Whether Create opens a public room on the Ember ID service instead of a private one.
    bool roomPublic = false;
    // Whose input delay a room created here plays at (room::Snapshot::delayPolicy):
    // each fighter their own unless the player chose Shared.
    room::DelayPolicy roomDelayPolicy = room::DelayPolicy::Own;
    // How many tables a private Classic room created here opens with (room::Snapshot::tableCount).
    int roomTableCount = static_cast<int>(room::ClassicTableCount);
    // Whether a room created here, public or private, is for wired players only (room::Snapshot::wiredOnly).
    bool roomWiredOnly = false;
    room::Rules tableRules;
    // The rules a public room is created with. Its tables open at the public
    // default; the creator, once in as host, sets these.
    room::Rules publicTableRules = room::PublicRoomRules();
    std::vector<RoomTemplate> roomTemplates;
    bool Valid() const {
        if (roomTemplates.size() > MaxRoomTemplates || !std::all_of(roomTemplates.begin(), roomTemplates.end(), [](const RoomTemplate& saved) { return saved.Valid(); })) return false;
        if (buttonLayouts.size() > 32 || !std::all_of(buttonLayouts.begin(), buttonLayouts.end(), [](const gamepads::StoredLayout& layout) { return gamepads::ValidLayout(layout.buttons) && gamepads::ValidBoundButtons(layout.bound) && gamepads::ValidExtraCopies(layout.extras); })) return false;
        if (!gamepads::Valid(buttonPrompts) || displayName.empty() || displayName.size() >= 32 || !ember::ValidFighter(mainFighter) || inputDelay < MinimumInputDelay || inputDelay > MaximumInputDelay || autoDelayStyle < 0 || autoDelayStyle >= AutoDelayStyles ||
            matchHudSize < 0 || matchHudSize > 2 || matchHudAnchor < 0 || matchHudAnchor > MaxMatchHudAnchor || matchHudLayout < 0 || matchHudLayout > 1 || pictureSpot < 0 || pictureSpot > 11 || pictureMode < PictureSmall || pictureMode > PictureOff || roomColumn < MinRoomColumn || roomColumn > MaxRoomColumn || roomSplit < MinRoomSplit || roomSplit > MaxRoomSplit || matchHudNameOffset < -MaxMatchHudNameOffset || matchHudNameOffset > MaxMatchHudNameOffset || readySoundVolume < 10 || readySoundVolume > 100 || !(interfaceScale >= 1.f && interfaceScale <= 1.5f) ||
            !lobby.Valid() ||
            roomName.empty() || roomName.size() > 64 || roomCapacity < 2 || roomCapacity > static_cast<int>(room::MaxMembers) ||
            roomTableCount < 1 || roomTableCount > static_cast<int>(room::TableCount)) return false;
        for (unsigned char c : displayName) if (c < 32 || c == 127) return false;
        for (unsigned char c : roomName) if (c < 32 || c == 127) return false;
        return country.Valid() && speech.Valid() && roomFilter.Valid() && tableRules.Valid() && publicTableRules.Valid();
    }
};

// Share my replays' cutoff once `sharing` is taken over a cutoff of `since`
// (0 while it was off): off clears it; on keeps the one it has, or starts one
// `now`: just turned on, or on at load with none (the default-on first run,
// or settings saved before there was a cutoff).
inline std::uint64_t ShareReplaysSince(bool sharing, std::uint64_t since, std::uint64_t now) {
    return !sharing ? 0 : since ? since : now;
}

// The player's edited preferences as the game thread takes them over
// `current`, in one step: the match record stays the game thread's, and the
// sharing cutoff follows Share my replays, never the edited copy's.
inline PlayerPreferences AcceptedPreferences(const PlayerPreferences& current, PlayerPreferences wanted, std::uint64_t now) {
    wanted.record = current.record;
    wanted.shareReplaysSince = ShareReplaysSince(wanted.shareReplays, current.shareReplays ? current.shareReplaysSince : 0, now);
    return wanted;
}

} }
