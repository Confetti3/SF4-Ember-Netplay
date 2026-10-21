#include "../common/ButtonLayoutJson.hxx"
#include "../common/ButtonLayoutSlots.hxx"
#include "../common/ButtonLayoutCapture.hxx"
#include "../common/XInputMapping.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "test_support.hxx"
#include <algorithm>
#include <cstring>

using namespace sf4e::gamepads;
static Identity Pad(unsigned n) { return {"guid" + std::to_string(n), "serial" + std::to_string(n), "path", "Pad"}; }
static void Model() {
    CHECK(DefaultLayout == ButtonLayout({0x40, 0x80, 0x10, 0x20, 0x430, 0x400, 0x8C0, 0x800, 0, 0}));
    CHECK(LayoutPresets[1].buttons == ButtonLayout({0x40, 0x80, 0x10, 0x20, 0x800, 0x400, 0x430, 0x8C0, 0, 0}));
    CHECK(LayoutPresets[2].buttons == ButtonLayout({0x40, 0x80, 0x10, 0x20, 0xA0, 0x400, 0x50, 0x800, 0x430, 0x8C0}));
    for (const auto& preset : LayoutPresets) {
        CHECK(ValidLayout(preset.buttons) && PresetOf(preset.buttons) == &preset);
        auto changed = preset.buttons; changed[0] = 0;
        CHECK(!PresetOf(changed));
    }
    const std::pair<unsigned, const char*> names[] = {{0x10, "LP"}, {0x430, "3P"}, {0x8C0, "3K"}, {0x50, "LP+LK"},
        {0xA0, "MP+MK"}, {0xB0, "LP+MP+MK"}, {0xC00, "HP+HK"}, {0x70, "LP+MP+LK"}, {0, ""}};
    for (const auto& name : names) CHECK(FightName(name.first) == name.second);
    auto layout = DefaultLayout;
    auto swap = Assign(layout, 3, 0x10); CHECK(swap.changed && swap.swapped == 2 && swap.old == 0x20);
    CHECK(layout[2] == 0x20 && layout[3] == 0x10 && layout[4] == 0x430);
    layout = DefaultLayout; Assign(layout, 5, 0x430); CHECK(layout[4] == 0x400 && layout[5] == 0x430);
    layout = DefaultLayout; Assign(layout, 6, 0x50); CHECK(layout[6] == 0x50 && layout[0] == 0x40);
    layout = DefaultLayout; CHECK(!Assign(layout, 2, 0x10).changed);
    layout[3] = 0; Assign(layout, 3, 0x10); CHECK(layout[2] == 0);
    for (unsigned mask = 0; mask <= FightMask; ++mask) if (!(mask & ~FightMask)) { SetMask(layout, 0, mask); CHECK(layout[0] == mask); }
    CHECK(!SetMask(layout, 0, 0x1000));
    SetMask(layout, 0, 0xA0); SetMask(layout, 1, 0xA0); CHECK(layout[0] == layout[1]);
    SetMask(layout, 0, 0); CHECK(layout[0] == 0);
    layout = DefaultLayout; CHECK(MissingAttacks(layout, AllLayoutButtons).empty());
    layout[2] = 0; CHECK(MissingAttacks(layout, AllLayoutButtons) == std::vector<std::uint32_t>{0x10});
    layout[2] = 0x50; CHECK(MissingAttacks(layout, AllLayoutButtons) == std::vector<std::uint32_t>{0x10});
    auto listed = AllLayoutButtons; listed[2] = false; layout = DefaultLayout;
    CHECK(MissingAttacks(layout, listed) == std::vector<std::uint32_t>{0x10});
    layout[2] = 0; layout[6] = 0x10; listed = AllLayoutButtons; listed[6] = false;
    CHECK(MissingAttacks(layout, listed) == std::vector<std::uint32_t>{0x10});
    Assign(layout, 3, 0x10, listed); CHECK(layout[6] == 0x10);
    layout = DefaultLayout;
    Assign(layout, 5, 0x10); const auto relayout = NamesOf(layout);
    CHECK(relayout[5] == "LP" && relayout[2] == "HP");
    SetMask(layout, 0, 0); CHECK(NamesOf(layout)[0].empty());
}
static void Persistence() {
    const std::vector<StoredLayout> original = {{Pad(1), LayoutPresets[1].buttons, 123}, {Pad(2), LayoutPresets[2].buttons, 456}};
    unsigned rejected = 0;
    const auto read = [&](const nlohmann::json& json) { return ReadLayouts(json, [&](unsigned, sf4e::gamepads::LayoutLoadError) { ++rejected; }); };
    CHECK(read({}).empty());
    auto json = nlohmann::json{{"buttonLayouts", LayoutsToJson(original)}};
    CHECK(read(json) == original);
    auto wrong = json["buttonLayouts"][0]; wrong["buttons"].erase(9); json["buttonLayouts"].push_back(wrong);
    wrong = json["buttonLayouts"][0]; wrong["buttons"][0] = 0x1000; json["buttonLayouts"].push_back(wrong);
    CHECK(read(json) == original && rejected == 2);
    auto layouts = original;
    for (unsigned n = 3; n <= 32; ++n) UpsertLayout(layouts, {Pad(n), DefaultLayout, static_cast<std::int64_t>(n + 1000)});
    CHECK(UpsertLayout(layouts, {Pad(33), DefaultLayout, 9999}) == original[0].identity && layouts.size() == 32);
    sf4e::netplay::PlayerPreferences prefs; prefs.buttonLayouts = original; CHECK(prefs.Valid());
    prefs.buttonLayouts[0].buttons[0] = 0x1000; CHECK(!prefs.Valid());
    prefs.buttonLayouts.assign(33, original[0]); CHECK(!prefs.Valid());
    auto pad = Pad(1); pad.guid = "bluetooth"; CHECK(FindLayout(original, pad, {pad}).index == 0);
    auto port = original; port[0].identity.serial.clear(); pad = port[0].identity; CHECK(FindLayout(port, pad, {pad}).index == 0);
    pad.path = "new-port"; CHECK(FindLayout(port, pad, {pad}).byGuid);
    CHECK(!FindLayout(port, pad, {pad, pad}).index);
    auto other = pad; other.path = "other-port"; CHECK(!FindLayout(port, pad, {other}).index);
    port.push_back(port[0]); CHECK(!FindLayout(port, pad, {pad}).index);
}
static void ExtraButtons() {
    // A layout saved before extra buttons existed loads with every extra button off.
    unsigned rejected = 0;
    const auto read = [&](const nlohmann::json& json) { return ReadLayouts(json, [&](unsigned, LayoutLoadError) { ++rejected; }); };
    StoredLayout layout{Pad(1), DefaultLayout, 5};
    auto json = nlohmann::json{{"buttonLayouts", LayoutsToJson({layout})}};
    json["buttonLayouts"][0].erase("extraButtons");
    CHECK(read(json).at(0).extras == NoExtraCopies && rejected == 0);
    layout.extras = {2, -1, 9, -1, -1, 0, -1};
    json = nlohmann::json{{"buttonLayouts", LayoutsToJson({layout})}};
    CHECK(read(json) == std::vector<StoredLayout>{layout});
    // Any bad copy or a bad list turns every extra button off. The layout still loads.
    json["buttonLayouts"][0]["extraButtons"][0] = CopyTargetCount;
    CHECK(read(json).at(0).extras == NoExtraCopies);
    json["buttonLayouts"][0]["extraButtons"][0] = 1.5;
    CHECK(read(json).at(0).extras == NoExtraCopies);
    json["buttonLayouts"][0]["extraButtons"] = {1, 2};
    CHECK(read(json).at(0).extras == NoExtraCopies);
    json["buttonLayouts"][0]["extraButtons"] = "touchpad";
    CHECK(read(json).at(0).extras == NoExtraCopies && rejected == 0);
    sf4e::netplay::PlayerPreferences prefs; prefs.buttonLayouts = {layout}; CHECK(prefs.Valid());
    prefs.buttonLayouts[0].extras[1] = CopyTargetCount; CHECK(!prefs.Valid());
    prefs.buttonLayouts[0].extras[1] = -2; CHECK(!prefs.Valid());
    // A copy of a button the pad lacks copies nothing.
    auto listed = AllLayoutButtons; listed[2] = false;
    CHECK(CopiedButton(layout.extras, 0, AllLayoutButtons) == 2u && !CopiedButton(layout.extras, 0, listed) && !CopiedButton(layout.extras, 1, AllLayoutButtons));
    // Each held extra button presses the layout button it copies; triggers press fully.
    State state; state.extras[static_cast<unsigned>(Extra::Touchpad)] = true;
    CHECK(MapToXInput(WithExtraCopies(state, NoExtraCopies)) == MapToXInput(state));
    ExtraCopies copies = NoExtraCopies; copies[static_cast<unsigned>(Extra::Touchpad)] = 2;
    CHECK(MapToXInput(WithExtraCopies(state, copies)).buttons == 0x4000);
    copies[static_cast<unsigned>(Extra::Touchpad)] = 7;
    CHECK(MapToXInput(WithExtraCopies(state, copies)).rightTrigger == 255 && MapToXInput(WithExtraCopies(state, copies)).buttons == 0);
    copies[static_cast<unsigned>(Extra::Touchpad)] = 9;
    CHECK(MapToXInput(WithExtraCopies(state, copies)).buttons == 0x0080);
    // An extra button that is not held, or copies nothing, presses nothing; a held copy stays one press.
    copies = {0, 1, 2, 3, 4, 5, 6};
    CHECK(MapToXInput(WithExtraCopies(State{}, copies)) == XInputFields{});
    state = {}; state.buttons[static_cast<unsigned>(Button::South)] = true; state.extras[0] = true; copies = NoExtraCopies; copies[0] = 0;
    CHECK(MapToXInput(WithExtraCopies(state, copies)).buttons == 0x1000);
    const std::pair<int, std::uint16_t> bits[] = {{0, 0x1000}, {1, 0x2000}, {2, 0x4000}, {3, 0x8000}, {4, 0x0100}, {5, 0x0200}, {8, 0x0040}, {9, 0x0080}};
    for (const auto& bit : bits) {
        state = {}; state.extras[static_cast<unsigned>(Extra::LeftPaddle2)] = true; copies = NoExtraCopies; copies[static_cast<unsigned>(Extra::LeftPaddle2)] = bit.first;
        CHECK(MapToXInput(WithExtraCopies(state, copies)).buttons == bit.second);
    }
    state = {}; state.extras[static_cast<unsigned>(Extra::Home)] = true; copies = NoExtraCopies; copies[static_cast<unsigned>(Extra::Home)] = 6;
    CHECK(MapToXInput(WithExtraCopies(state, copies)).leftTrigger == 255);
    // View and Start are copies too, whatever the pad lists, and survive a save.
    copies = NoExtraCopies; copies[static_cast<unsigned>(Extra::Touchpad)] = CopiesView; copies[static_cast<unsigned>(Extra::Home)] = CopiesStart;
    state = {}; state.extras[static_cast<unsigned>(Extra::Touchpad)] = true; state.extras[static_cast<unsigned>(Extra::Home)] = true;
    CHECK(MapToXInput(WithExtraCopies(state, copies)).buttons == (0x0020 | 0x0010));
    CHECK(CopiedButton(copies, 0, listed) == static_cast<unsigned>(CopiesView) && CopiedButton(copies, 1, listed) == static_cast<unsigned>(CopiesStart));
    CHECK(!std::strcmp(CopyTargetGlyph(CopiesView), "View") && !std::strcmp(CopyTargetGlyph(CopiesStart), "Start") && !std::strcmp(CopyTargetGlyph(2), "X"));
    layout.extras = copies; json = nlohmann::json{{"buttonLayouts", LayoutsToJson({layout})}};
    CHECK(read(json).at(0).extras == copies);
}
static void TrainingButtons() {
    // A layout saved before Training buttons existed loads with both off.
    unsigned rejected = 0;
    const auto read = [&](const nlohmann::json& json) { return ReadLayouts(json, [&](unsigned, LayoutLoadError) { ++rejected; }); };
    const auto training = [](const StoredLayout& layout) { return std::make_pair(layout.bound[TrainingRecordButton], layout.bound[TrainingPlayButton]); };
    const auto off = std::make_pair(-1, -1);
    StoredLayout layout{Pad(1), DefaultLayout, 5};
    auto json = nlohmann::json{{"buttonLayouts", LayoutsToJson({layout})}};
    json["buttonLayouts"][0].erase("trainingRecord"); json["buttonLayouts"][0].erase("trainingPlay");
    CHECK(training(read(json).at(0)) == off && rejected == 0);
    layout.bound[TrainingRecordButton] = 9; layout.bound[TrainingPlayButton] = 8; layout.bound[MenuConfirmButton] = 2;
    json = nlohmann::json{{"buttonLayouts", LayoutsToJson({layout})}};
    CHECK(read(json) == std::vector<StoredLayout>{layout});
    // An index out of range, or one button for both, turns the pair off and
    // leaves the menu pair alone; a value that is no integer is that button off.
    // The layout still loads.
    json["buttonLayouts"][0]["trainingRecord"] = 10;
    CHECK(training(read(json).at(0)) == off && read(json).at(0).bound[MenuConfirmButton] == 2);
    json["buttonLayouts"][0]["trainingRecord"] = 1.5;
    CHECK(training(read(json).at(0)) == std::make_pair(-1, 8));
    json["buttonLayouts"][0]["trainingRecord"] = 8;
    CHECK(training(read(json).at(0)) == off);
    json["buttonLayouts"][0]["trainingRecord"] = "RS";
    CHECK(training(read(json).at(0)) == std::make_pair(-1, 8) && rejected == 0);
    // A menu button and a Training button may share one: only partners may not.
    json["buttonLayouts"][0]["trainingRecord"] = 2;
    CHECK(training(read(json).at(0)) == std::make_pair(2, 8));
    sf4e::netplay::PlayerPreferences prefs; prefs.buttonLayouts = {layout}; CHECK(prefs.Valid());
    const auto set = [&](int record, int play) { prefs.buttonLayouts[0].bound[TrainingRecordButton] = record; prefs.buttonLayouts[0].bound[TrainingPlayButton] = play; };
    set(8, 8); CHECK(!prefs.Valid());
    set(10, -1); CHECK(!prefs.Valid());
    set(-1, -2); CHECK(!prefs.Valid());
    set(-1, -1); CHECK(prefs.Valid());
    // The stick clicks are bits 24 and 25; none for no button or one the pad lacks.
    CHECK(LayoutButtonMask(8) == 0x1000000u && LayoutButtonMask(9) == 0x2000000u && LayoutButtonMask(-1) == 0);
    auto listed = AllLayoutButtons; listed[8] = false;
    CHECK(LayoutButtonMask(8, listed) == 0);
    // Offline Training's table: the Training buttons do nothing in the fight
    // there, whatever the layout gives them; the menu buttons and the rest keep theirs.
    auto bound = NoBoundButtons; bound[TrainingRecordButton] = 8; bound[TrainingPlayButton] = 9; bound[MenuConfirmButton] = 2;
    auto table = DefaultLayout; table[8] = 0x430; table[9] = 0x8C0;
    auto expected = table; expected[8] = expected[9] = 0;
    CHECK(TrainingTable(table, bound) == expected && TrainingTable(table, NoBoundButtons) == table);
}
static void Writer() {
    alignas(std::uint32_t) std::array<unsigned char, PadBindingRecords::Stride * 6> memory;
    memory.fill(0x5A); const auto untouched = memory;
    PadBindingRecords records(memory.data(), memory.data() + memory.size());
    ButtonLayout game; CHECK(ReadSlot(records, 0, game));
    LayoutOwners owners; LayoutPlacements placements; std::array<bool, 4> occupied{};
    const auto apply = [&] { const auto plan = SlotWritePlan(owners, placements, occupied); CHECK(ApplySlotWrites(owners, placements, plan, records)); return plan; };
    placements[0] = LayoutPlacement{Pad(1), DefaultLayout}; occupied[0] = true;
    CHECK(apply()[0].copyFirst && owners[0]->gameCopy == game);
    ButtonLayout read; ReadSlot(records, 0, read); CHECK(read == DefaultLayout);
    // Disconnect retains the table until a replacement actually occupies it.
    occupied[0] = false; placements[0].reset(); CHECK(apply().empty());
    placements[2] = LayoutPlacement{Pad(1), LayoutPresets[1].buttons}; occupied[2] = true;
    CHECK(apply().size() == 1); ReadSlot(records, 0, read); CHECK(read == DefaultLayout);
    occupied[0] = true; CHECK(apply()[0].source == LayoutSource::GameCopy); ReadSlot(records, 0, read); CHECK(read == game);
    CHECK(apply().empty());
    placements[2].reset(); CHECK(apply()[0].source == LayoutSource::GameCopy);
    placements[1] = LayoutPlacement{Pad(2), DefaultLayout}; occupied[1] = true; apply();
    placements[1]->buttons[0] = 0x50; CHECK(apply().size() == 1);
    placements[1]->buttons[0] = 0x10;
    const auto beforeInvalid = memory; const auto beforeOwners = owners;
    placements[3] = LayoutPlacement{Pad(3), DefaultLayout}; occupied[3] = true;
    const auto plan = SlotWritePlan(owners, placements, occupied);
    PadBindingRecords shortRecords(memory.data(), memory.data() + 3 * PadBindingRecords::Stride);
    CHECK(!ApplySlotWrites(owners, placements, plan, shortRecords) && memory == beforeInvalid);
    CHECK(!owners[3]);
    PadBindingRecords reversed(memory.data() + 4, memory.data()); CHECK(!WriteSlot(reversed, 0, DefaultLayout));
    CHECK(!WriteSlot(records, 4, DefaultLayout));
    for (unsigned record = 0; record < 6; ++record) for (unsigned offset = 0; offset < PadBindingRecords::Stride; ++offset) {
        bool bindable = false;
        if (record < 4) for (auto bit : LayoutBits) if (offset >= 0x17C + bit * 4 && offset < 0x17C + (bit + 1) * 4) bindable = true;
        if (!bindable) CHECK(memory[record * PadBindingRecords::Stride + offset] == untouched[record * PadBindingRecords::Stride + offset]);
    }
    // A new connection in the same reserved slot is a placement, even with the same identity.
    placements[3].reset(); occupied[3] = false;
    placements[1]->instance = 19; CHECK(apply().size() == 1);
    // The game's edit copy now contains only its own tables, across two owners.
    placements[0] = LayoutPlacement{Pad(1), DefaultLayout}; apply();
    GameTableSwap screen; CHECK(screen.Open(GameTableUse::ButtonScreen, owners, records) && screen.Use() == GameTableUse::ButtonScreen);
    for (unsigned slot : {0u, 1u}) { ReadSlot(records, slot, read); CHECK(read == owners[slot]->gameCopy); }
    auto newGame = DefaultLayout; newGame[2] = 0x50; WriteSlot(records, 0, newGame);
    auto otherGame = DefaultLayout; otherGame[3] = 0xA0; WriteSlot(records, 1, otherGame);
    CHECK(screen.Close(owners, records) && !screen.Use());
    CHECK(owners[0]->gameCopy == newGame && owners[1]->gameCopy == otherGame);
    ReadSlot(records, 0, read); CHECK(read == owners[0]->buttons); ReadSlot(records, 1, read); CHECK(read == owners[1]->buttons);
    // Shutdown restores every owned slot, including a disconnected owner's slot.
    const auto beforeShutdown = memory;
    PadBindingRecords shutdownShort(memory.data(), memory.data() + PadBindingRecords::Stride);
    CHECK(!RestoreGameLayouts(owners, shutdownShort) && memory == beforeShutdown && owners[0] && owners[1]);
    CHECK(RestoreGameLayouts(owners, records));
    CHECK(ReadSlot(records, 0, read) && read == newGame && !owners[0]);
    CHECK(ReadSlot(records, 1, read) && read == otherGame && !owners[1]);
}
// The game's config load (.ctrlsav) rewrites owned tables after Ember wrote them.
static void Reload() {
    alignas(std::uint32_t) std::array<unsigned char, PadBindingRecords::Stride * 4> memory; memory.fill(0);
    PadBindingRecords records(memory.data(), memory.data() + memory.size());
    for (unsigned slot = 0; slot < 4; ++slot) WriteSlot(records, slot, DefaultLayout);
    LayoutOwners owners; LayoutPlacements placements; std::array<bool, 4> occupied{};
    const auto apply = [&] { CHECK(ApplySlotWrites(owners, placements, SlotWritePlan(owners, placements, occupied), records)); };
    placements[0] = LayoutPlacement{Pad(1), LayoutPresets[1].buttons}; placements[2] = LayoutPlacement{Pad(2), LayoutPresets[2].buttons};
    occupied[0] = occupied[2] = true; apply();
    GameTableSwap tables; ButtonLayout read;
    // The loader runs on the game's own tables, and what it leaves is kept.
    CHECK(tables.Open(GameTableUse::ConfigLoad, owners, records));
    for (unsigned slot : {0u, 2u}) CHECK(ReadSlot(records, slot, read) && read == DefaultLayout);
    auto loaded = DefaultLayout; loaded[0] = 0x50; WriteSlot(records, 0, loaded);
    auto unowned = DefaultLayout; unowned[1] = 0xA0; WriteSlot(records, 1, unowned);
    CHECK(tables.Close(owners, records) && !tables.Use());
    CHECK(owners[0]->gameCopy == loaded && ReadSlot(records, 0, read) && read == LayoutPresets[1].buttons);
    CHECK(owners[2]->gameCopy == DefaultLayout && ReadSlot(records, 2, read) && read == LayoutPresets[2].buttons);
    CHECK(!owners[1] && ReadSlot(records, 1, read) && read == unowned);
    // A later remove writes back the reloaded table, not the one saved at placement.
    placements[0].reset(); apply();
    CHECK(!owners[0] && ReadSlot(records, 0, read) && read == loaded);
    // Bounds failure: slot 2 lies outside these records, so nothing is written
    // and the tables stay with Ember until a Close succeeds.
    auto before = memory;
    PadBindingRecords shortRecords(memory.data(), memory.data() + 2 * PadBindingRecords::Stride);
    CHECK(!tables.Open(GameTableUse::ConfigLoad, owners, shortRecords) && memory == before && !tables.Use());
    CHECK(tables.Open(GameTableUse::ConfigLoad, owners, records)); WriteSlot(records, 2, loaded); before = memory;
    CHECK(!tables.Close(owners, shortRecords) && memory == before && tables.Use() == GameTableUse::ConfigLoad);
    CHECK(tables.Close(owners, records) && owners[2]->gameCopy == loaded && ReadSlot(records, 2, read) && read == LayoutPresets[2].buttons);
    // A load while the button screen holds the tables is left to the screen's Close.
    CHECK(tables.Open(GameTableUse::ButtonScreen, owners, records) && !tables.Open(GameTableUse::ConfigLoad, owners, records));
    auto edited = loaded; edited[4] = 0x20; WriteSlot(records, 2, edited);
    CHECK(tables.Use() == GameTableUse::ButtonScreen && tables.Close(owners, records) && owners[2]->gameCopy == edited);
}
// A loaded table equal to the Ember layout still replaces the startup table.
static void ReloadEqual() {
    alignas(std::uint32_t) std::array<unsigned char, PadBindingRecords::Stride * 4> memory; memory.fill(0);
    PadBindingRecords records(memory.data(), memory.data() + memory.size());
    for (unsigned slot = 0; slot < 4; ++slot) WriteSlot(records, slot, DefaultLayout);
    LayoutOwners owners; LayoutPlacements placements; std::array<bool, 4> occupied{};
    const auto apply = [&] { CHECK(ApplySlotWrites(owners, placements, SlotWritePlan(owners, placements, occupied), records)); };
    const auto layout = LayoutPresets[1].buttons;
    placements[0] = LayoutPlacement{Pad(1), layout}; occupied[0] = true; apply();
    CHECK(owners[0]->gameCopy == DefaultLayout);
    GameTableSwap tables; ButtonLayout read;
    CHECK(tables.Open(GameTableUse::ConfigLoad, owners, records));
    WriteSlot(records, 0, layout);
    CHECK(tables.Close(owners, records) && owners[0]->gameCopy == layout && ReadSlot(records, 0, read) && read == layout);
    // The button screen opened after the load shows the loaded table.
    CHECK(tables.Open(GameTableUse::ButtonScreen, owners, records) && ReadSlot(records, 0, read) && read == layout);
    CHECK(tables.Close(owners, records));
    // Removing the layout restores the loaded table, not the startup one.
    placements[0].reset(); apply();
    CHECK(!owners[0] && ReadSlot(records, 0, read) && read == layout);
}
static void Capture() {
    LayoutCapture capture; capture.Begin(0, {"A"});
    CHECK(!capture.Read(1, {"A"}, AllLayoutButtons).button); capture.Read(2, {}, AllLayoutButtons);
    CHECK(capture.Read(3, {"Y"}, AllLayoutButtons).button == 3);
    for (const auto& held : {std::vector<std::string>{"Start"}, std::vector<std::string>{"left"}}) {
        capture.Begin(0, {}); CHECK(capture.Read(1, held, AllLayoutButtons).cancelled != LayoutCancel::None);
    }
    for (auto reason : {LayoutCancel::Escape, LayoutCancel::Back, LayoutCancel::State}) {
        capture.Begin(0, {}); CHECK(capture.Read(1, {}, AllLayoutButtons, reason).cancelled == reason);
    }
    capture.Begin(0, {}); CHECK(capture.Left(0) == 1 && capture.Left(2.5) == .75 && capture.Left(12) == 0);
    CHECK(capture.Read(10, {}, AllLayoutButtons).cancelled == LayoutCancel::Timeout && capture.Left(10) == 0);
}
int main() { Model(); Persistence(); ExtraButtons(); TrainingButtons(); Writer(); Reload(); ReloadEqual(); Capture(); }
