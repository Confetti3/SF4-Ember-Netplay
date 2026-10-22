#pragma once
#include <string>
#include <map>
#include <vector>
#include "../common/ButtonPrompts.hxx"
#include "../common/ButtonLayoutTypes.hxx"
#include "../common/Country.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/InputDelay.hxx"
#include "../common/RoomRules.hxx"
#include "ProfileRecord.hxx"

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
// The V2 room board's layout grips (Customise.dc.html), in percent: the members
// and chat column's share of the board's width, and the members' share of that
// column over chat. Each snaps to its step from the default.
constexpr int MinRoomColumn = 28, MaxRoomColumn = 60, DefaultRoomColumn = 36, RoomColumnStep = 4;
constexpr int MinRoomSplit = 26, MaxRoomSplit = 74, DefaultRoomSplit = 56, RoomSplitStep = 6;

struct PlayerPreferences {
    std::string displayName = "Player";
    int mainFighter = 0;
    ProfileRecord record;
    int inputDelay = DefaultInputDelay;
    // Auto readies with the connection check's delay (AutoInputDelay) instead of
    // inputDelay. It is on until the player chooses a number.
    bool autoInputDelay = true;
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
    // Ember's V2 look (Settings > Interface > Look); off keeps the classic one.
    bool lookV2 = false;
    // Called out of Training to their table, the player is readied at once
    // instead of being given the time to ready themselves.
    bool trainingAutoReady = false;
    // Ember's menu over a game the player watches shows it small in its top
    // right corner over an opaque menu, instead of dimmed behind the menu.
    bool watchedPicture = false;
    // The training frame meter over the player's own matches and those they
    // watch. It only reads the game and shows confirmed frames.
    bool matchFrameMeter = false;
    // Percent of the game's own voice volume, in steps of ten.
    int readySoundVolume = 100;
    // The game's sound and the pads keep working while another window, such
    // as OBS, is in front (sf4e__BackgroundPlay.cxx).
    bool backgroundPlay = false;
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
    // Room members can open this player's Steam profile from the room's member
    // screen. Sent when they join a room; nothing is sent when it is off.
    bool shareSteamProfile = true;
    // The flag beside this player's name in rooms and on the player card:
    // Windows' region (Automatic), a country they picked, or Hidden. Sent when
    // they join a room; nothing is sent while it is Hidden or Windows names none.
    CountryPreference country;
    bool discordPresence = true, discordInvites = true;
    // The launcher sends a crash's report without asking, logs only and at most
    // three a day (platform/ReportWorkflow.hxx). Off until the player turns it
    // on in Settings, Problem reports, or chooses Always send after a crash.
    bool sendProblemReports = false;
    float interfaceScale = 1.f;
    LobbySettings lobby;
    std::string roomName = "Private room";
    int roomCapacity = 16;
    // Whether Create opens a public room on the Ember ID service instead of a private one.
    bool roomPublic = false;
    // Whose input delay a room created here plays at (room::Snapshot::delayPolicy).
    room::DelayPolicy roomDelayPolicy = room::DelayPolicy::Shared;
    room::Rules tableRules;
    // The rules a public room is created with. Its tables open at the public
    // default; the creator, once in as host, sets these.
    room::Rules publicTableRules = room::PublicRoomRules();
    bool Valid() const {
        if (buttonLayouts.size() > 32 || !std::all_of(buttonLayouts.begin(), buttonLayouts.end(), [](const gamepads::StoredLayout& layout) { return gamepads::ValidLayout(layout.buttons) && gamepads::ValidMenuBinding(layout.menu) && gamepads::ValidExtraCopies(layout.extras); })) return false;
        if (!gamepads::Valid(buttonPrompts) || displayName.empty() || displayName.size() >= 32 || mainFighter<0 || mainFighter>=selection::FighterCount || inputDelay < MinimumInputDelay || inputDelay > MaximumInputDelay ||
            matchHudSize < 0 || matchHudSize > 2 || matchHudAnchor < 0 || matchHudAnchor > MaxMatchHudAnchor || matchHudLayout < 0 || matchHudLayout > 1 || pictureSpot < 0 || pictureSpot > 11 || roomColumn < MinRoomColumn || roomColumn > MaxRoomColumn || roomSplit < MinRoomSplit || roomSplit > MaxRoomSplit || matchHudNameOffset < -MaxMatchHudNameOffset || matchHudNameOffset > MaxMatchHudNameOffset || readySoundVolume < 10 || readySoundVolume > 100 || !(interfaceScale >= 1.f && interfaceScale <= 1.5f) || !lobby.Valid() ||
            roomName.empty() || roomName.size() > 64 || roomCapacity < 2 || roomCapacity > static_cast<int>(room::MaxMembers)) return false;
        for (unsigned char c : displayName) if (c < 32 || c == 127) return false;
        for (unsigned char c : roomName) if (c < 32 || c == 127) return false;
        return country.Valid() && tableRules.Valid() && publicTableRules.Valid();
    }
};

} }
