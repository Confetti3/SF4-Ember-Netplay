#include "CardEditRows.hxx"
#include "CardModel.hxx"
#include "CardParts.hxx"
#include "MenuRows.hxx"
#include "NetworkFeedback.hxx"
#include "PublicRoomsPanel.hxx"
#include "RoomCards.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cstdint>

namespace sf4e { namespace ui { namespace card_edit {
namespace aw = netplay::awards;
namespace pr = netplay::profiles;
namespace {
// The reason a row gives in a room is the one Edit card's other rows give.
std::string Detail(bool canEdit, const char* key) { return canEdit ? loc::T(key) : loc::T("profile.leave_room_to_edit"); }

// A device's, or a looking for choice's, name as the card's chips say it.
std::string DeviceName(pr::Device device) {
    pr::Style style;
    style.device = device;
    const auto chips = player_card::PlaysChips(style);
    return chips.empty() ? std::string() : chips.front().text;
}
std::string LookingName(std::size_t kind) {
    pr::Style style;
    style.looking = pr::LookingBit[kind];
    const auto chips = player_card::LookingChips(style);
    return chips.empty() ? std::string() : chips.front().text;
}

// A switch: On or Off, which Select flips (the row's hint says so) and Left and Right set.
MenuEntry Switch(std::string id, std::string label, bool on, std::string detail, bool enabled) {
    auto row = Value(std::move(id), std::move(label), loc::T(on ? "common.on" : "common.off"), std::move(detail), enabled);
    row.opens = true;
    row.hint = loc::T("menu.change");
    return row;
}

bool HasPrefix(const std::string& id, const char* prefix) { return id.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; }

// A catalog family's level reached, 0 for none.
int Reached(const aw::Counters& counters, const aw::Family& family) { return aw::LevelOf(counters, family); }
// The family's current title at the level reached ("World Warrior"), for a family with one.
std::string TitleOf(const aw::Counters& counters, const aw::Family& family) {
    const aw::Level* level = aw::LevelAt(family, Reached(counters, family));
    return level ? player_card::LevelTitle(*level) : std::string();
}

// The place of a Looking for choice in pr::LookingBit, or the kinds' count for none.
std::size_t LookingKindOf(const std::string& id) {
    if (!HasPrefix(id, LookingPrefix)) return pr::LookingKinds;
    const std::string number = id.substr(std::char_traits<char>::length(LookingPrefix));
    return number.size() == 1 && number[0] >= '0' && number[0] < '0' + static_cast<char>(pr::LookingKinds) ? static_cast<std::size_t>(number[0] - '0') : pr::LookingKinds;
}

// `want` 1 on, 0 off, -1 flip.
bool Wanted(bool now, int want) { return want < 0 ? !now : want > 0; }

Change SetLooking(netplay::PlayerPreferences& prefs, std::size_t kind, int want) {
    Change change;
    change.handled = true;
    const std::uint8_t bit = pr::LookingBit[kind];
    const bool on = (prefs.cardLooking & bit) != 0, next = Wanted(on, want);
    if (next == on) return change;
    if (next && pr::LookingCount(prefs.cardLooking) >= pr::MaxLooking) { change.refusal = "card.looking_full"; return change; }
    prefs.cardLooking = static_cast<std::uint8_t>(next ? prefs.cardLooking | bit : prefs.cardLooking & ~bit);
    change.changed = true;
    return change;
}

// Automatic is no pins. Turning it off starts from the families the card shows now, so the list begins as the player sees it;
// untaking the last one gives the card back to Automatic.
Change SetAutomatic(netplay::PlayerPreferences& prefs, const Measured& measured, int want) {
    Change change;
    change.handled = true;
    const bool on = prefs.cardPins.empty(), next = Wanted(on, want);
    if (next == on) return change;
    if (next) { prefs.cardPins.clear(); change.changed = true; return change; }
    for (const auto& tile : player_card::ShownFamilies(measured.counters, {})) prefs.cardPins.push_back(tile.family->id);
    change.changed = !prefs.cardPins.empty();
    return change;
}

Change SetPin(netplay::PlayerPreferences& prefs, const Measured& measured, const aw::Family& family, int want) {
    Change change;
    change.handled = true;
    // The list is for choosing; while Automatic is on it is disabled.
    if (prefs.cardPins.empty()) return change;
    auto pins = ReachedPins(prefs, measured.counters);
    const auto at = std::find(pins.begin(), pins.end(), family.id);
    const bool on = at != pins.end(), next = Wanted(on, want);
    if (next == on) return change;
    if (next) {
        if (Reached(measured.counters, family) == 0) return change;
        if (pins.size() >= pr::MaxPins) { change.refusal = "card.pins_full"; return change; }
        pins.push_back(family.id);
    } else pins.erase(at);
    change.changed = pins != prefs.cardPins;
    prefs.cardPins = std::move(pins);
    return change;
}

Change SetSwitch(netplay::PlayerPreferences& prefs, const Measured& measured, const std::string& id, int want) {
    Change change;
    if (id == SteamRow) {
        change.handled = true;
        const bool next = Wanted(prefs.shareSteamProfile, want);
        change.changed = next != prefs.shareSteamProfile;
        prefs.shareSteamProfile = next;
        return change;
    }
    if (id == AutoPinsRow) return SetAutomatic(prefs, measured, want);
    if (const std::size_t kind = LookingKindOf(id); kind < pr::LookingKinds) return SetLooking(prefs, kind, want);
    if (const auto* family = FamilyOfRow(id)) return SetPin(prefs, measured, *family, want);
    return change;
}
}

Measured MeasuredOf(const ShellView& v, const aw::Counters& counters) {
    Measured measured;
    measured.automatic = pr::AutomaticDevice(v.inputDevice.type, v.inputDevice.connected, v.gameplayBrand);
    measured.link = v.ownLink;
    measured.region = PublicRoomsPanel::KnownRegion(v);
    measured.counters = counters;
    return measured;
}

std::vector<std::string> DeviceValues(pr::Device automatic) {
    return {automatic == pr::Device::None ? std::string(loc::T("card.automatic")) : loc::Tf("card.device_auto", DeviceName(automatic)),
        DeviceName(pr::Device::Pad), DeviceName(pr::Device::Stick), DeviceName(pr::Device::Leverless), DeviceName(pr::Device::Keyboard), loc::T("card.hidden")};
}

std::string DeviceValue(const netplay::PlayerPreferences& prefs, pr::Device automatic) {
    const auto values = DeviceValues(automatic);
    return values[prefs.cardDevice >= 0 && prefs.cardDevice < static_cast<int>(values.size()) ? prefs.cardDevice : static_cast<int>(values.size()) - 1];
}

std::string LinkValue(const netplay::PlayerPreferences& prefs, NetworkLink link) {
    if (!prefs.cardShowLink) return loc::T("card.hidden");
    if (link == NetworkLink::Wired) return loc::Tf("card.shown", loc::T("style.wired"));
    if (link == NetworkLink::Wireless) return loc::Tf("card.shown", loc::T("style.wifi"));
    return loc::T("card.unknown_shown");
}

std::string RegionValue(const netplay::PlayerPreferences& prefs, const std::string& region) {
    if (!prefs.cardShowRegion) return loc::T("card.hidden");
    const char* name = region.empty() ? nullptr : RelayRegionName(region);
    return name ? loc::Tf("card.shown", name) : std::string(loc::T("card.unknown_shown"));
}

std::string LookingValue(const netplay::PlayerPreferences& prefs) {
    std::string text;
    for (std::size_t i = 0; i < pr::LookingKinds; ++i)
        if (prefs.cardLooking & pr::LookingBit[i]) text += (text.empty() ? "" : ", ") + LookingName(i);
    return text.empty() ? std::string(loc::T("card.none")) : text;
}

std::vector<std::string> ReachedPins(const netplay::PlayerPreferences& prefs, const aw::Counters& counters) {
    std::vector<std::string> reached;
    for (const auto& id : prefs.cardPins) {
        const aw::Family* family = aw::Find(id);
        if (family && aw::Earned(counters, *family) && std::find(reached.begin(), reached.end(), id) == reached.end()) reached.push_back(id);
    }
    return reached;
}

std::string PinsValue(const netplay::PlayerPreferences& prefs, const aw::Counters& counters) {
    return prefs.cardPins.empty() ? std::string(loc::T("card.pins_auto")) : loc::Tf("card.pins_chosen", static_cast<int>(ReachedPins(prefs, counters).size()));
}

MenuEntry PinsEntry(const Context& c) {
    auto row = Row(PinsRow, loc::T("card.pins_row"), Detail(c.canEdit, "card.pins_detail"), c.canEdit);
    row.value = PinsValue(c.prefs, c.measured.counters);
    row.hint = loc::T("menu.choose");
    return row;
}

std::vector<MenuEntry> PlayEntries(const Context& c) {
    // A stepped row: Left and Right change it, and the hint bar says so once.
    const auto stepped = [&](const char* id, const char* label, std::string value, const char* detail) {
        auto row = Value(id, loc::T(label), std::move(value), Detail(c.canEdit, detail), c.canEdit);
        row.adjustHint = loc::T("menu.change");
        return row;
    };
    std::vector<MenuEntry> rows;
    rows.push_back(stepped(DeviceRow, "card.device_row", DeviceValue(c.prefs, c.measured.automatic), "card.device_detail"));
    rows.push_back(stepped(LinkRow, "card.link_row", LinkValue(c.prefs, c.measured.link), "card.link_detail"));
    rows.push_back(stepped(RegionRow, "card.region_row", RegionValue(c.prefs, c.measured.region), "card.region_detail"));
    auto looking = Row(LookingRow, loc::T("card.looking_row"), Detail(c.canEdit, "card.looking_detail"), c.canEdit);
    looking.value = LookingValue(c.prefs);
    looking.hint = loc::T("menu.choose");
    rows.push_back(std::move(looking));
    return rows;
}

MenuEntry SteamEntry(const Context& c) {
    // As Profile words it: the row keeps its own detail in a room.
    return Switch(SteamRow, loc::T("profile.share_steam"), c.prefs.shareSteamProfile, loc::T("profile.share_steam_detail"), c.canEdit);
}

std::vector<MenuEntry> LookingEntries(const Context& c) {
    std::vector<MenuEntry> rows;
    for (std::size_t i = 0; i < pr::LookingKinds; ++i)
        rows.push_back(Switch(LookingPrefix + std::to_string(i), LookingName(i), (c.prefs.cardLooking & pr::LookingBit[i]) != 0, Detail(c.canEdit, "card.looking_detail"), c.canEdit));
    return rows;
}

const aw::Family* FamilyOfRow(const std::string& id) {
    return HasPrefix(id, PinPrefix) ? aw::Find(id.substr(std::char_traits<char>::length(PinPrefix))) : nullptr;
}

std::vector<MenuEntry> PinEntries(const Context& c) {
    const auto& counters = c.measured.counters;
    const bool automatic = c.prefs.cardPins.empty();
    std::vector<const aw::Family*> reached;
    for (const auto& family : aw::Catalog) if (Reached(counters, family) > 0) reached.push_back(&family);
    std::vector<MenuEntry> rows;
    // Nothing to choose from says so, on the one row there is.
    const std::string autoDetail = !c.canEdit ? std::string(loc::T("profile.leave_room_to_edit")) :
        reached.empty() ? std::string(loc::T("card.pins_nothing")) : std::string(loc::T("card.pins_auto_detail"));
    rows.push_back(Switch(AutoPinsRow, loc::T("card.pins_auto_row"), automatic, autoDetail, c.canEdit && !reached.empty()));
    const auto chosen = ReachedPins(c.prefs, counters);
    for (const aw::Family* family : reached) {
        const bool on = std::find(chosen.begin(), chosen.end(), family->id) != chosen.end();
        // While Automatic is on the list is shown and cannot be changed; it says so in its detail.
        rows.push_back(Switch(std::string(PinPrefix) + family->id, loc::T(family->name), on,
            !c.canEdit ? std::string(loc::T("profile.leave_room_to_edit")) : automatic ? std::string(loc::T("card.pins_auto_detail")) : TitleOf(counters, *family),
            c.canEdit && !automatic));
    }
    return rows;
}

bool Owns(const std::string& id) {
    return id == DeviceRow || id == LinkRow || id == RegionRow || id == PinsRow || id == LookingRow || id == SteamRow || id == AutoPinsRow ||
        FamilyOfRow(id) != nullptr || LookingKindOf(id) < pr::LookingKinds;
}

Change Adjust(netplay::PlayerPreferences& prefs, const Measured& measured, const std::string& id, int delta) {
    Change change;
    if (id == DeviceRow) {
        change.handled = true;
        const int next = (std::max)(0, (std::min)(5, prefs.cardDevice + delta));
        change.changed = next != prefs.cardDevice;
        prefs.cardDevice = next;
    } else if (id == LinkRow || id == RegionRow) {
        change.handled = true;
        bool& shown = id == LinkRow ? prefs.cardShowLink : prefs.cardShowRegion;
        change.changed = shown != (delta > 0);
        shown = delta > 0;
    } else if (delta != 0) return SetSwitch(prefs, measured, id, delta > 0 ? 1 : 0);
    return change;
}

Change Press(netplay::PlayerPreferences& prefs, const Measured& measured, const std::string& id) {
    return SetSwitch(prefs, measured, id, -1);
}

bool DrawPinRow(const MenuEntry& row, const aw::Counters& counters, ImVec2 min, ImVec2 max) {
    const aw::Family* family = FamilyOfRow(row.id);
    if (!family) return false;
    const int level = Reached(counters, *family);
    if (level == 0) return false;
    const float s = Scale(), size = ImGui::GetFontSize(), height = max.y - min.y;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const ImU32 text = row.enabled ? c.text : v2::color::Alpha(c.text, .55f), muted = row.enabled ? c.muted : v2::color::Alpha(c.muted, .55f);
    // The icon on its tier's plate, no frame: a tier is never a frame.
    const float plate = (std::min)(40 * s, height - 12 * s);
    const ImVec2 at(min.x + 12 * s, min.y + (height - plate) * .5f);
    card_parts::DrawIconPlate(d, at, plate, plate * .6f, family->icon, v2::FrameTier(aw::LevelAt(*family, level)->tier));
    // The check at the right end: a box, filled with the accent and a tick when the family is chosen.
    const bool on = row.value == loc::T("common.on");
    const float box = 22 * s, right = max.x - 16 * s;
    const ImVec2 b0(right - box, min.y + (height - box) * .5f), b1(right, b0.y + box);
    if (on) {
        d->AddRectFilled(b0, b1, row.enabled ? c.accent : v2::color::Alpha(c.accent, .55f), 4 * s);
        const float line = (std::max)(2.f, 2 * s);
        d->AddLine(ImVec2(b0.x + box * .24f, b0.y + box * .52f), ImVec2(b0.x + box * .43f, b0.y + box * .72f), c.ink, line);
        d->AddLine(ImVec2(b0.x + box * .43f, b0.y + box * .72f), ImVec2(b0.x + box * .78f, b0.y + box * .30f), c.ink, line);
    } else d->AddRect(b0, b1, muted, 4 * s, 0, Hairline());
    // Its name over its current title, cut where they must be.
    const float x = at.x + plate + 12 * s, room = b0.x - 12 * s - x, detailSize = 15 * s;
    const float top = min.y + (height - size - detailSize - 4 * s) * .5f;
    std::string elided;
    DrawCardText(elided, ImVec2(x, top), room, row.label, size, text);
    DrawCardText(elided, ImVec2(x, top + size + 4 * s), room, TitleOf(counters, *family), detailSize, muted);
    return true;
}

} } }
