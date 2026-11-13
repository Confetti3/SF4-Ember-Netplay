// Edit card's rows past Name, Flag and Main (docs/design/PROFILE.md, "Edit
// card"): the words each value reads, Device stepping through its six values,
// Connection and Region shown or hidden, Looking for refusing a third choice,
// the pins page listing only families with a level and refusing a fifth,
// Automatic disabling the list, every row disabled in a room, and what the
// hint bar says for each kind of row.
#include "../ui/CardEditRows.hxx"
#include "../ui/GameMenu.hxx"
#include "../ui/MenuPresentation.hxx"
#include "../ui/MenuRows.hxx"
#include "../common/Localization.hxx"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sf4e;
namespace ce = ui::card_edit;
namespace pr = netplay::profiles;
namespace aw = netplay::awards;
int failures = 0;
void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

// The counters of a player who has reached the first level of every family.
aw::Counters Everything() {
    aw::Counters counters;
    counters.games = 1; counters.wins = 1; counters.bestStreak = 3; counters.peakClass = 5; counters.topFighterTier = 1;
    counters.fightersPlayed = 5; counters.friends = 1; counters.rival = 10; counters.opponents = 10;
    return counters;
}
// Two families reached: Matches (at its second level) and Wins.
aw::Counters Two() {
    aw::Counters counters;
    counters.games = 120; counters.wins = 3;
    return counters;
}

ce::Measured Measures(pr::Device automatic, aw::Counters counters) {
    ce::Measured measured;
    measured.automatic = automatic;
    measured.link = NetworkLink::Wired;
    measured.region = "euc1";
    measured.counters = counters;
    return measured;
}

const ui::MenuEntry* Find(const std::vector<ui::MenuEntry>& rows, const std::string& id) {
    for (const auto& row : rows) if (row.id == id) return &row;
    return nullptr;
}

std::vector<std::string> Pinned(const std::vector<ui::MenuEntry>& rows) {
    std::vector<std::string> ids;
    for (const auto& row : rows) if (ce::FamilyOfRow(row.id) && row.value == loc::T("common.on")) ids.push_back(ce::FamilyOfRow(row.id)->id);
    return ids;
}

// The words of every value, as Edit card says them.
void ValueWords() {
    netplay::PlayerPreferences prefs;
    const std::string dot = "\xC2\xB7";
    // Automatic names the controller in use; with none known it says only Automatic.
    Check(ce::DeviceValue(prefs, pr::Device::Stick) == loc::Tf("card.device_auto", loc::T("style.stick")), "Automatic does not name the controller in use");
    Check(ce::DeviceValue(prefs, pr::Device::Stick).find(dot) != std::string::npos, "Automatic is not worded with its dot");
    Check(ce::DeviceValue(prefs, pr::Device::None) == loc::T("card.automatic"), "Automatic with no controller known does not say Automatic");
    prefs.cardDevice = 3;
    Check(ce::DeviceValue(prefs, pr::Device::Stick) == loc::T("style.leverless"), "A chosen device is not named");
    prefs.cardDevice = 5;
    Check(ce::DeviceValue(prefs, pr::Device::Stick) == loc::T("card.hidden"), "Not shown does not read Hidden");
    prefs.cardDevice = 9;
    Check(ce::DeviceValue(prefs, pr::Device::Stick) == loc::T("card.hidden"), "A device this build does not know is not Hidden");
    Check(ce::DeviceValues(pr::Device::Stick).size() == 6, "Device has not six values");

    // The game measures the connection and the region; the player only shows or hides them.
    Check(ce::LinkValue(prefs, NetworkLink::Wired) == loc::Tf("card.shown", loc::T("style.wired")), "A wired connection does not read 'Wired · shown'");
    Check(ce::LinkValue(prefs, NetworkLink::Wireless) == loc::Tf("card.shown", loc::T("style.wifi")), "A wireless connection does not read 'Wi-Fi · shown'");
    Check(ce::LinkValue(prefs, NetworkLink::Unknown) == loc::T("card.unknown_shown"), "An unmeasured connection does not read 'Not known yet · shown'");
    Check(ce::RegionValue(prefs, "euc1") == loc::Tf("card.shown", loc::T("network.region_euc1")), "A region does not read 'Europe · shown'");
    Check(ce::RegionValue(prefs, "") == loc::T("card.unknown_shown"), "No region known does not read 'Not known yet · shown'");
    prefs.cardShowLink = prefs.cardShowRegion = false;
    Check(ce::LinkValue(prefs, NetworkLink::Wired) == loc::T("card.hidden") && ce::RegionValue(prefs, "euc1") == loc::T("card.hidden"), "A hidden connection or region does not read Hidden");

    // Looking for lists its choices in the order a screen does, and None for nothing.
    Check(ce::LookingValue(prefs) == loc::T("card.none"), "Looking for nothing does not read None");
    prefs.cardLooking = pr::LookingTeach | pr::LookingLongSets;
    Check(ce::LookingValue(prefs) == std::string(loc::T("style.long_sets")) + ", " + loc::T("style.teach"), "Looking for does not list its choices in order");
    // Awards on card: the highest, or how many are chosen.
    prefs.cardPins.clear();
    Check(ce::PinsValue(prefs, Two()) == loc::T("card.pins_auto"), "No pins does not read Your highest");
    prefs.cardPins = {"matches", "wins"};
    Check(ce::PinsValue(prefs, Two()) == loc::Tf("card.pins_chosen", 2), "Two pins do not read '2 chosen'");
}

// Device steps Automatic, Pad, Stick, Leverless, Keyboard, Hidden and stops at both ends.
void DeviceSteps() {
    netplay::PlayerPreferences prefs;
    const auto measured = Measures(pr::Device::Stick, {});
    const auto values = ce::DeviceValues(pr::Device::Stick);
    Check(ce::DeviceValue(prefs, pr::Device::Stick) == values[0], "Device does not start on Automatic");
    for (int step = 1; step < 6; ++step) {
        const auto before = prefs;
        const auto change = ce::Adjust(prefs, measured, ce::DeviceRow, 1);
        Check(change.handled && change.changed && !change.refusal && prefs.cardDevice == step, "Right did not step Device to the next value");
        Check(ce::DeviceValue(prefs, pr::Device::Stick) == values[step], "Device does not read the value it stepped to");
        Check(!ui::SamePreferences(before, prefs), "A Device step is not a change to save");
    }
    const auto end = ce::Adjust(prefs, measured, ce::DeviceRow, 1);
    Check(end.handled && !end.changed && prefs.cardDevice == 5, "Device stepped past Hidden");
    for (int step = 4; step >= 0; --step) ce::Adjust(prefs, measured, ce::DeviceRow, -1);
    Check(prefs.cardDevice == 0 && !ce::Adjust(prefs, measured, ce::DeviceRow, -1).changed, "Device did not step back to Automatic and stop");
    Check(prefs.Valid(), "A device step made the preferences invalid");
    // With no controller known Automatic is still the first value.
    Check(ce::DeviceValues(pr::Device::None)[0] == loc::T("card.automatic"), "Automatic with no controller is not the first value");
}

// Connection and Region are switched shown and hidden by Right and Left; neither can be typed.
void ConnectionAndRegion() {
    netplay::PlayerPreferences prefs;
    const auto measured = Measures(pr::Device::None, {});
    Check(ce::Adjust(prefs, measured, ce::LinkRow, -1).changed && !prefs.cardShowLink, "Left did not hide the connection");
    Check(!ce::Adjust(prefs, measured, ce::LinkRow, -1).changed, "Hiding a hidden connection changed it");
    Check(ce::Adjust(prefs, measured, ce::LinkRow, 1).changed && prefs.cardShowLink, "Right did not show the connection");
    Check(ce::Adjust(prefs, measured, ce::RegionRow, -1).changed && !prefs.cardShowRegion, "Left did not hide the region");
    Check(ce::Adjust(prefs, measured, ce::RegionRow, 1).changed && prefs.cardShowRegion, "Right did not show the region");
    // Select on a stepped row is not this module's: only the arrows change it.
    Check(!ce::Press(prefs, measured, ce::DeviceRow).handled && !ce::Press(prefs, measured, ce::LinkRow).handled, "Select changed a stepped row");
}

// Looking for is at most two: a third is refused with its words and changes nothing.
void LookingRefusesAThird() {
    netplay::PlayerPreferences prefs;
    const auto measured = Measures(pr::Device::None, {});
    const std::string casuals = std::string(ce::LookingPrefix) + "0", ranked = std::string(ce::LookingPrefix) + "2", teach = std::string(ce::LookingPrefix) + "5";
    Check(ce::Press(prefs, measured, casuals).changed && ce::Press(prefs, measured, ranked).changed, "Two looking-for choices did not turn on");
    const auto before = prefs;
    const auto refused = ce::Press(prefs, measured, teach);
    Check(refused.handled && !refused.changed && refused.refusal && std::string(refused.refusal) == "card.looking_full", "A third choice was not refused with words");
    Check(prefs.cardLooking == before.cardLooking && ui::SamePreferences(prefs, before), "A refused third choice changed the preferences");
    Check(ce::Adjust(prefs, measured, teach, 1).refusal != nullptr, "Right on a third choice was not refused");
    // Turning one off lets another on.
    Check(ce::Press(prefs, measured, casuals).changed && (prefs.cardLooking & pr::LookingCasuals) == 0, "A choice did not turn off");
    Check(ce::Press(prefs, measured, teach).changed && (prefs.cardLooking & pr::LookingTeach) != 0 && prefs.Valid(), "A choice did not turn on once one was off");
    Check(ce::Adjust(prefs, measured, teach, -1).changed && (prefs.cardLooking & pr::LookingTeach) == 0, "Left did not turn a choice off");
    // The page lists the six, in LookingBit's order, each a switch.
    const auto rows = ce::LookingEntries({prefs, measured, true});
    Check(rows.size() == pr::LookingKinds, "Looking for is not six choices");
    Check(rows[0].value == loc::T("common.off") && rows[2].value == loc::T("common.on"), "Looking for does not show which are on");
    Check(!ce::Press(prefs, measured, std::string(ce::LookingPrefix) + "6").handled && !ce::Press(prefs, measured, std::string(ce::LookingPrefix) + "x").handled, "A seventh choice is handled");
}

// The pins page lists families with a level reached and nothing else, refuses a fifth, and Automatic disables the list.
void PinsListAndLimit() {
    netplay::PlayerPreferences prefs;
    // Two families reached: only they are listed, after the Automatic row.
    auto measured = Measures(pr::Device::None, Two());
    auto rows = ce::PinEntries({prefs, measured, true});
    Check(rows.size() == 3 && rows[0].id == ce::AutoPinsRow && rows[1].id == std::string(ce::PinPrefix) + "matches" && rows[2].id == std::string(ce::PinPrefix) + "wins",
        "The pins page does not list the families with a level, after Automatic");
    Check(Find(rows, std::string(ce::PinPrefix) + "streak") == nullptr, "A family with no level is listed");
    // Automatic is on by default and disables the list, with its detail.
    Check(rows[0].value == loc::T("common.on") && rows[0].enabled && !rows[1].enabled && !rows[2].enabled, "Automatic does not disable the list");
    Check(rows[1].detail == loc::T("card.pins_auto_detail"), "A disabled family does not say why");
    Check(!ce::Press(prefs, measured, rows[1].id).changed && prefs.cardPins.empty(), "A family was chosen while Automatic was on");
    // Turning Automatic off starts from what the card shows now.
    Check(ce::Press(prefs, measured, ce::AutoPinsRow).changed && prefs.cardPins.size() == 2, "Turning Automatic off did not start from the families shown");
    rows = ce::PinEntries({prefs, measured, true});
    Check(rows[0].value == loc::T("common.off") && rows[1].enabled && rows[2].enabled && Pinned(rows).size() == 2, "The list is not open with Automatic off");
    // Taking the last one off gives the card back to Automatic.
    ce::Press(prefs, measured, rows[1].id);
    Check(prefs.cardPins.size() == 1, "A family did not come off");
    ce::Press(prefs, measured, rows[2].id);
    Check(prefs.cardPins.empty() && ce::PinEntries({prefs, measured, true})[0].value == loc::T("common.on"), "Taking every family off did not return to Automatic");

    // Nothing reached: the Automatic row says so and nothing can be chosen.
    measured = Measures(pr::Device::None, {});
    rows = ce::PinEntries({prefs, measured, true});
    Check(rows.size() == 1 && !rows[0].enabled && rows[0].detail == loc::T("card.pins_nothing"), "With no award reached the page does not say so");

    // Five or more families: four at most, a fifth refused with words.
    measured = Measures(pr::Device::None, Everything());
    Check(ce::Press(prefs, measured, ce::AutoPinsRow).changed && prefs.cardPins.size() == pr::MaxPins, "Automatic off did not start from four families");
    rows = ce::PinEntries({prefs, measured, true});
    Check(rows.size() == 1 + aw::CatalogSize && Pinned(rows).size() == 4, "The page does not list every family with four chosen");
    std::string fifth;
    for (const auto& row : rows) if (ce::FamilyOfRow(row.id) && row.value == loc::T("common.off")) { fifth = row.id; break; }
    const auto before = prefs;
    const auto refused = ce::Press(prefs, measured, fifth);
    Check(!fifth.empty() && refused.handled && !refused.changed && refused.refusal && std::string(refused.refusal) == "card.pins_full", "A fifth family was not refused with words");
    Check(ui::SamePreferences(prefs, before) && prefs.cardPins.size() == 4, "A refused fifth family changed the preferences");
    // One off, then another on.
    ce::Press(prefs, measured, std::string(ce::PinPrefix) + prefs.cardPins.front());
    Check(prefs.cardPins.size() == 3, "A chosen family did not come off");
    Check(ce::Press(prefs, measured, fifth).changed && prefs.cardPins.size() == 4 && prefs.cardPins.back() == ce::FamilyOfRow(fifth)->id && prefs.Valid(), "A family did not go on once there was room");
}

// A chosen family nobody reached is skipped: it is not counted, and the first change drops it.
void StalePins() {
    netplay::PlayerPreferences prefs;
    prefs.cardPins = {"streak", "matches"};
    const auto measured = Measures(pr::Device::None, Two());
    Check(ce::ReachedPins(prefs, measured.counters) == std::vector<std::string>({"matches"}), "A family with no level is counted as chosen");
    Check(ce::PinsValue(prefs, measured.counters) == loc::Tf("card.pins_chosen", 1), "A family with no level is in the count of chosen");
    Check(ce::Press(prefs, measured, std::string(ce::PinPrefix) + "wins").changed && prefs.cardPins == std::vector<std::string>({"matches", "wins"}), "The first change did not drop the family nobody reached");
    // A family that does not exist is not this module's row.
    Check(!ce::Press(prefs, measured, std::string(ce::PinPrefix) + "organizer").handled, "A family that is no award is handled");
}

// In a room every row is disabled, with the words Edit card's other rows give.
void RoomDisablesEverything() {
    netplay::PlayerPreferences prefs;
    const auto measured = Measures(pr::Device::Stick, Everything());
    const ce::Context room{prefs, measured, false}, open{prefs, measured, true};
    std::vector<ui::MenuEntry> rows = ce::PlayEntries(room);
    rows.push_back(ce::PinsEntry(room));
    rows.push_back(ce::SteamEntry(room));
    for (auto& row : ce::LookingEntries(room)) rows.push_back(std::move(row));
    for (auto& row : ce::PinEntries(room)) rows.push_back(std::move(row));
    Check(rows.size() == 4 + 1 + 1 + 6 + 1 + aw::CatalogSize, "Edit card's rows are not all built");
    for (const auto& row : rows) {
        Check(!row.enabled, "A card row is enabled in a room");
        Check(row.id == ce::SteamRow || row.detail == loc::T("profile.leave_room_to_edit"), "A card row does not say to leave the room");
    }
    // Outside a room the same rows are enabled (Automatic's list is the one exception).
    rows = ce::PlayEntries(open);
    rows.push_back(ce::PinsEntry(open));
    rows.push_back(ce::SteamEntry(open));
    for (auto& row : ce::LookingEntries(open)) rows.push_back(std::move(row));
    for (const auto& row : rows) Check(row.enabled, "A card row is disabled outside a room");
    const auto pins = ce::PinEntries(open);
    Check(pins[0].enabled && !pins[1].enabled, "The pins page is not Automatic enabled and its list off");
}

// What the hint bar says by kind of row: Choose where Select opens something, only Change on the arrows of a stepped row,
// Change on the switch's Select; never two hints that say the same.
void HintBarWords() {
    netplay::PlayerPreferences prefs;
    const auto measured = Measures(pr::Device::Stick, Two());
    const ce::Context card{prefs, measured, true};
    const std::string choose = loc::T("menu.choose"), change = loc::T("menu.change");
    const auto words = [](const ui::MenuEntry& row) {
        const char* select = ui::MenuPrimaryHint(&row);
        return std::make_pair(std::string(select ? select : ""), ui::MenuAdjustHint(row));
    };
    const auto pins = words(ce::PinsEntry(card));
    Check(pins.first == choose && pins.second.empty(), "Awards on card does not say only Choose");
    const auto play = ce::PlayEntries(card);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto stepped = words(play[i]);
        Check(stepped.first.empty() && stepped.second == change, "A stepped row does not say only the arrows' Change");
    }
    const auto looking = words(play[3]);
    Check(looking.first == choose && looking.second.empty(), "Looking for does not say only Choose");
    const auto steam = words(ce::SteamEntry(card));
    Check(steam.first == change && steam.second.empty(), "A switch does not say only Select's Change");
    for (const auto& row : ce::LookingEntries(card)) {
        const auto pair = words(row);
        Check(pair.first == change && pair.second.empty(), "A looking-for switch does not say only Change");
    }
    auto open = prefs;
    open.cardPins = {"matches"};
    for (const auto& row : ce::PinEntries({open, measured, true})) {
        const auto pair = words(row);
        Check(pair.first == change && pair.second.empty(), "A pins switch does not say only Change");
    }
    // A disabled row says nothing at all, and no row says one word twice.
    prefs = netplay::PlayerPreferences();
    for (const auto& row : ce::PlayEntries({prefs, measured, false})) Check(words(row).first.empty() && words(row).second.empty(), "A disabled row has hints");
    for (const auto& row : ce::PlayEntries(card)) {
        const auto pair = words(row);
        Check(pair.first.empty() || pair.second.empty() || pair.first != pair.second, "A row says the same word twice");
    }
}

// What a press changes is a change to save, and what it refuses is not.
void ChangesSave() {
    netplay::PlayerPreferences prefs;
    const auto measured = Measures(pr::Device::None, Everything());
    const std::string rows[] = {ce::SteamRow, std::string(ce::LookingPrefix) + "1", ce::AutoPinsRow};
    for (const auto& id : rows) {
        const auto before = prefs;
        const auto change = ce::Press(prefs, measured, id);
        Check(change.handled && change.changed && !ui::SamePreferences(before, prefs) && prefs.Valid(), "A press that changed a card row is not a change to save");
    }
    Check(!ce::Press(prefs, measured, "card-title").handled && !ce::Owns("card-title") && ce::Owns(ce::PinsRow) && ce::Owns(ce::AutoPinsRow), "Rows are claimed that are not the card's");
}
}

int main() {
    ValueWords();
    DeviceSteps();
    ConnectionAndRegion();
    LookingRefusesAThird();
    PinsListAndLimit();
    StalePins();
    RoomDisablesEverything();
    HintBarWords();
    ChangesSave();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("card edit tests passed\n");
    return 0;
}
