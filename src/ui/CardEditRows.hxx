#pragma once
// Edit card's rows past Name, Flag, Main, Title and Icon (docs/design/PROFILE.md,
// "Edit card"): how the player plays (Device, Connection, Region, Looking for),
// the award families the card shows (Awards on card and its page), the
// Looking for page and the Steam switch. What each row reads and what a press
// does are plain functions of the preferences and what the game measures, so
// they are tested with no frame (card_edit_test.cxx); the Settings screens
// (ApplicationShell__Settings.cxx) only wire them to the shell's save.
//
// - Device steps through six values (Automatic, Pad, Stick, Leverless,
//   Keyboard, Hidden: PlayerPreferences::cardDevice 0 to 5). Connection and
//   Region are shown or hidden: the game measures them, the player never types.
// - Looking for is a page of the six choices, at most two on. Awards on card is
//   a page: Automatic (the highest), or up to four families that have a level
//   reached. A press that would pass a limit changes nothing and says why.
// - Every row is disabled in a room, with the reason Edit card's other rows give.
#include "MenuNavigation.hxx"
#include "../common/NetworkLink.hxx"
#include "../netplay/Awards.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/Profiles.hxx"
#include <imgui.h>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
namespace card_edit {

// The two pages over Edit card (their rows' ids are the screens' names).
constexpr const char* PinsScreen = "card-pins";
constexpr const char* LookingScreen = "card-looking";
// Edit card's rows. PinsRow and LookingRow open their pages.
constexpr const char* DeviceRow = "card-device";
constexpr const char* LinkRow = "card-link";
constexpr const char* RegionRow = "card-region";
constexpr const char* PinsRow = "card-pins";
constexpr const char* LookingRow = "card-looking";
constexpr const char* SteamRow = "share-steam";
// The pages' rows: the Automatic switch and a family, a Looking for choice by its place in LookingBit.
constexpr const char* AutoPinsRow = "pins-auto";
constexpr const char* PinPrefix = "pin:";
constexpr const char* LookingPrefix = "looking:";

inline bool OnCardScreen(const std::string& screen) { return screen == "card-edit" || screen == PinsScreen || screen == LookingScreen; }

// What the game measures now: the device Automatic stands for, the connection,
// the relay region Ember picked (empty for none or an unnamed one) and the
// counters that say which award families have a level.
struct Measured {
    netplay::profiles::Device automatic = netplay::profiles::Device::None;
    NetworkLink link = NetworkLink::Unknown;
    std::string region;
    netplay::awards::Counters counters;
};
// As the card's preview and the card save read them (ProfilePanel::OwnSubjectOf).
Measured MeasuredOf(const ShellView& view, const netplay::awards::Counters& counters);

// The words of each value.
std::string DeviceValue(const netplay::PlayerPreferences& prefs, netplay::profiles::Device automatic);
// Every value of the Device row in the order Left and Right step through them.
std::vector<std::string> DeviceValues(netplay::profiles::Device automatic);
std::string LinkValue(const netplay::PlayerPreferences& prefs, NetworkLink link);
std::string RegionValue(const netplay::PlayerPreferences& prefs, const std::string& region);
// "Long sets, Happy to teach", or None.
std::string LookingValue(const netplay::PlayerPreferences& prefs);
// "Your highest", or "3 chosen".
std::string PinsValue(const netplay::PlayerPreferences& prefs, const netplay::awards::Counters& counters);

// The chosen families that have a level reached, in the order chosen: the ones
// the card can show, and the ones a press counts.
std::vector<std::string> ReachedPins(const netplay::PlayerPreferences& prefs, const netplay::awards::Counters& counters);

struct Context {
    const netplay::PlayerPreferences& prefs;
    const Measured& measured;
    // Whether the preferences can change now (not in a room).
    bool canEdit;
};

// Awards on card and the four rows of How you play, as Edit card lists them.
MenuEntry PinsEntry(const Context& context);
std::vector<MenuEntry> PlayEntries(const Context& context);
// Show my Steam profile: a switch Select flips.
MenuEntry SteamEntry(const Context& context);
// The pages: Looking for's six switches, and Awards on card's Automatic switch
// then a row for each family with a level reached (the words alone when none).
std::vector<MenuEntry> LookingEntries(const Context& context);
std::vector<MenuEntry> PinEntries(const Context& context);

// Whether `id` is one of this module's rows.
bool Owns(const std::string& id);

// What a press did. `handled`: the row is this module's. `refusal`: the
// catalog id of the words that say why nothing changed (a limit), else null.
struct Change {
    bool handled = false, changed = false;
    const char* refusal = nullptr;
};
// Left or Right on a row: Device steps, Connection and Region are shown (Right)
// or hidden (Left), a switch is turned on (Right) or off (Left).
Change Adjust(netplay::PlayerPreferences& prefs, const Measured& measured, const std::string& id, int delta);
// Select on a switch flips it. A row that opens a page is not handled here.
Change Press(netplay::PlayerPreferences& prefs, const Measured& measured, const std::string& id);

// The family a pins row names, or null.
const netplay::awards::Family* FamilyOfRow(const std::string& id);
// A pins row drawn as the Awards wall draws a family: its icon on its tier plate,
// its name and current title, a check at the right end. False for any other row.
bool DrawPinRow(const MenuEntry& row, const netplay::awards::Counters& counters, ImVec2 min, ImVec2 max);

} } }
