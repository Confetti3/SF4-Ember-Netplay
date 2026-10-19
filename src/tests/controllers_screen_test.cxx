#include "../ui/ControllersScreen.hxx"
#include "../ui/ButtonLayoutScreen.hxx"
#include "../netplay/ControllerInventory.hxx"
#include "../common/ControllerHints.hxx"
#include "test_support.hxx"

using namespace sf4e;
using namespace sf4e::gamepads;
using namespace sf4e::ui;
static PadView View(unsigned id, RowState state, int slot = -1) {
    PadView pad; pad.id = id; pad.connectionOrder = id; pad.state = state; pad.slot = slot;
    pad.name = "Pad " + std::to_string(id); pad.identity = {std::to_string(id), "", pad.name, pad.name};
    return pad;
}
static void Rows() {
    std::vector<PadView> pads = {View(8, RowState::Absent), View(7, RowState::NotInUse), View(6, RowState::Unmapped),
        View(4, RowState::Recognised, 3), View(3, RowState::Steam, 2), View(2, RowState::Recognised, 1), View(1, RowState::Recognised, 0), View(5, RowState::Unmapped)};
    pads[2].directInputIndex = 6; pads.back().directInputIndex = 4;
    OrderPadRows(pads);
    for (unsigned i = 0; i < pads.size(); ++i) CHECK(pads[i].id == i + 1);
    auto pad = pads.front(); pad.gameplay = true;
    auto row = ControllerRow(pad, true, true);
    CHECK(row.id == "pad-1" && row.userText && row.detailText == DetailText::Name);
    CHECK(row.value == "Yours" && row.detail == loc::Tf("controllers.detail_yours", 1));
    CHECK(row.choices.size() == 3 && row.choices[0].id == "play" && !row.choices[0].enabled);
    CHECK(row.choices[0].detail == loc::T("controllers.choice_play_current"));
    CHECK(row.choices[2].id == "setup" && row.choices[2].enabled);
    pad.gameplay = false; pad.customMapping = true;
    row = ControllerRow(pad, true, true);
    CHECK(row.value.empty() && row.detail == loc::Tf("controllers.detail_other", 1) + "\n" + loc::T("controllers.detail_custom"));
    CHECK(row.choices.size() == 4 && row.choices[3].id == "forget" && row.choices[3].enabled);
    row = ControllerRow(pad, false, false);
    CHECK(!row.choices[0].enabled && row.choices[0].detail == loc::T("settings.leave_room_to_edit"));
    CHECK(!row.choices[2].enabled && row.choices[2].detail == loc::T("settings.leave_room_to_edit"));
    CHECK(row.choices[1].enabled && !row.choices[3].enabled && row.choices[3].detail == loc::T("settings.leave_room_to_edit"));
    pad.customMapping = false; pad.state = RowState::Steam;
    row = ControllerRow(pad, true, true);
    CHECK(row.value == "Steam Input" && row.detail == loc::T("controllers.detail_steam"));
    CHECK(!row.choices[2].enabled && row.choices[2].detail == loc::T("controllers.choice_setup_steam"));
    pad.state = RowState::Unmapped;
    row = ControllerRow(pad, false, true);
    CHECK(row.value == "Not set up" && row.detail == loc::T("controllers.detail_unmapped") && row.choices.empty() && row.enabled && !row.info);
    row = ControllerRow(pad, true, false);
    CHECK(!row.enabled && row.detail == loc::T("settings.leave_room_to_edit"));
    pad.state = RowState::NotInUse; row = ControllerRow(pad, true, true);
    CHECK(row.info && row.value == "Not in use" && row.detail == loc::T("controllers.detail_full"));
    pad.state = RowState::Absent; row = ControllerRow(pad, true, true);
    CHECK(row.info && row.value == "Not connected" && row.detail == loc::T("controllers.detail_absent"));
    pad.state = RowState::GameRead; pad.gameplay = true;
    row = ControllerRow(pad, true, true, Layer::GameReads);
    CHECK(row.info && row.value == "Yours" && row.detail == loc::T("controllers.detail_game_read") && row.choices.empty());
    row = ControllerRow(pad, true, true, Layer::Active);
    CHECK(!row.info && row.choices.size() == 2 && row.choices[0].id == "layout" && !row.choices[0].enabled && row.choices[1].id == "setup");
    CHECK(ControllerRows(Layer::Active, {}, true, true)[0].id == "pads-none");
    const auto off = ControllerRows(Layer::GameReads, {}, true, true);
    CHECK(off.size() == 2 && off[0].id == "pads-game" && off[1].id == "pads-none");
    CHECK(ControllerRows(Layer::Failed, {pad}, true, true)[0].id == "pads-unavailable");
    CHECK(LayerPending(false, true) && LayerPending(true, false) && !LayerPending(true, true) && !LayerPending(false, false));
    auto player = PlayerControllerRows({}, "DualSense", input::PadXInput, {Brand::PlayStation}, pads, true, true, true, true);
    const char* ids[] = {"name", "controllers", "capture", "keyboard", "button-prompts", "controls", "game-buttons", "background-play", "lower-input-lag", "lower-keyboard-lag", "faster-display", "borderless", "game-reads-pads"};
    CHECK(player.size() == std::size(ids));
    for (unsigned i = 0; i < player.size(); ++i) CHECK(player[i].id == ids[i]);
    CHECK(player[1].value == "7 connected" && !player[1].adjustable && player[1].detailText == DetailText::Name && player[2].detailText == DetailText::Name);
    CHECK(player[4].value == "Automatic" && player[4].detail.find("\nShowing PlayStation buttons.") != std::string::npos);
    CHECK(player.back().value == "Off" && player.back().detail.find("\n\nRestart the game to apply this.") != std::string::npos);
    const auto locked = PlayerControllerRows({}, "Pad", input::PadKeyboard, {}, {}, true, false, false, false);
    CHECK(locked[1].enabled && !locked[2].enabled && !locked[4].enabled && !locked[5].enabled);
    CHECK(!locked.back().enabled && locked.back().detail == loc::T("settings.leave_room_to_edit"));
    CHECK(locked[4].detail == loc::T("settings.leave_room_to_edit"));
    MenuAction action; action.id = "pad-1"; action.kind = MenuAction::Chosen; action.text = "unknown";
    CHECK(!ControllerAction(action, pads));
    action.text = "play"; CHECK(ControllerAction(action, pads)->kind == ControllerRequest::Kind::Play);
    action.id = "pad-99"; CHECK(!ControllerAction(action, pads));
    pads = {View(1, RowState::NotInUse), View(2, RowState::NotInUse)};
    pads[0].connectionOrder = 30; pads[1].connectionOrder = 20;
    OrderPadRows(pads); CHECK(pads[0].id == 2 && pads[1].id == 1);
}
static void LayoutEntryStates() {
    auto pad = View(1, RowState::Recognised, 0); pad.gameplay = true; pad.brand = {Brand::PlayStation, PlayStationArt::PS5};
    const auto player = [&](int device, Layer layer) {
        return PlayerControllerRows({}, pad.name, device, pad.brand, {pad}, false, false, false, false, layer)[5];
    };
    auto row = player(input::PadXInput, Layer::Active);
    CHECK(row.enabled && row.value == loc::T("button_layout.game_layout") && row.detail == loc::Tf("player.button_layout_detail", pad.name));
    pad.layout = DefaultLayout; row = player(input::PadXInput, Layer::Active); CHECK(row.value == loc::T("button_layout.preset.default"));
    pad.state = RowState::GameRead; row = player(input::PadDirectInput, Layer::Active);
    CHECK(row.enabled && row.value.empty() && row.detail == loc::Tf("player.button_layout_setup", pad.name));
    row = player(input::PadKeyboard, Layer::Active); CHECK(!row.enabled && row.quiet && row.detail == loc::T("player.button_layout_keyboard"));
    row = player(input::PadXInput, Layer::GameReads); CHECK(!row.enabled && row.detail == loc::T("player.button_layout_game_reads"));
    row = player(input::PadXInput, Layer::Failed); CHECK(!row.enabled && row.detail == loc::T("player.button_layout_failed"));
    pad.brand = {Brand::Stick}; pad.numberedHats = true;
    using T = SdlBindType;
    pad.bindings = {{{T::Button, 3}, SdlTarget("a")}, {{T::Hat, 1, 0, 0, 2}, SdlTarget("b")}, {{T::Axis, 0, 0, SdlAxisMax}, SdlTarget("x")},
        {{T::Axis, 1, 0, SdlAxisMin}, SdlTarget("y")}, {{T::Axis, 2, SdlAxisMax, SdlAxisMin}, SdlTarget("leftshoulder")},
        {{T::Hat, 0, 0, 0, 3}, SdlTarget("rightshoulder")}, {{T::Axis, 4, SdlAxisMax, 0}, SdlTarget("lefttrigger")}};
    const auto physical = LayoutButtonNames(pad, ButtonPrompts::Automatic);
    CHECK(physical[0] == "Button 4" && physical[1] == "Hat right 2" && physical[2] == "Axis 1 +" && physical[3] == "Axis 2 -" && physical[4] == "Trigger axis 3");
    CHECK(physical[5] == "RB" && physical[6] == "Axis 5 +");
    CHECK(LayoutButtonNames(pad, ButtonPrompts::Nintendo)[0] == "B");
    netplay::PlayerPreferences a, b; a.buttonLayouts = b.buttonLayouts = {{pad.identity, DefaultLayout, 1}};
    CHECK(SamePreferences(a, b)); b.buttonLayouts[0].buttons[0] = 0; CHECK(!SamePreferences(a, b) && OnlyLayoutsChanged(a, b));
}
static void Held() {
    State state;
    CHECK(HeldGlyphs(state).empty());
    state.leftTrigger = state.rightTrigger = 16383; state.leftX = state.leftY = 16383;
    CHECK(HeldGlyphs(state).empty());
    state.leftTrigger = state.rightTrigger = 16384; state.leftX = state.leftY = 16384;
    auto held = HeldGlyphs(state);
    CHECK(held == std::vector<std::string>({"down", "right", "LT", "RT"}));
    state = {}; state.leftX = state.leftY = -16383; CHECK(HeldGlyphs(state).empty());
    state.leftX = state.leftY = -16384; CHECK(HeldGlyphs(state).empty());
    state.leftX = state.leftY = -16385;
    CHECK(HeldGlyphs(state) == std::vector<std::string>({"up", "left"}));
    state = {};
    for (auto& button : state.buttons) button = true;
    CHECK(HeldGlyphs(state) == std::vector<std::string>({"up", "down", "left", "right", "A", "B", "X", "Y", "LB", "RB", "LS", "RS", "View", "Start"}));
    RawState raw; raw.buttons = {false, false, false, true}; raw.hats = {1};
    raw.axes = {16000, -16000, -32768, 32767}; raw.rest = {0, 0, -32768, 32767};
    CHECK(RawInputList(HeldRaw(raw)) == "Button 4, Hat up");
    raw.axes = {16001, -16001, 32767, -32768}; raw.hats = {3, 12};
    CHECK(RawInputList(HeldRaw(raw)) == "Button 4, Hat up 1, Hat right 1, Hat down 2, Hat left 2, Axis 1 +, Axis 2 -, Trigger axis 3, Trigger axis 4");
}
static void Hints() {
    for (auto context : {HintContext::HiddenMainMenu, HintContext::OpenMainMenu, HintContext::Room, HintContext::Fight, HintContext::NativeMenu})
        for (auto kind : {HintKind::Connected, HintKind::Adopted, HintKind::Keyboard, HintKind::Unmapped, HintKind::Full, HintKind::Reconnected, HintKind::Disconnected})
            CHECK(ShowPadHint(context, kind) == (context == HintContext::HiddenMainMenu || (context == HintContext::OpenMainMenu && kind != HintKind::Disconnected)));
    for (const auto state : {RowState::Recognised, RowState::Steam, RowState::Unmapped, RowState::NotInUse}) {
        ControllerHints hints; auto pad = View(1, state);
        CHECK(hints.Update({}, false, HintContext::HiddenMainMenu, 0).empty());
        CHECK(hints.Update({pad}, false, HintContext::HiddenMainMenu, 10).empty());
        CHECK(hints.Update({pad}, false, HintContext::HiddenMainMenu, 1009).empty());
        const auto ready = hints.Update({pad}, false, HintContext::HiddenMainMenu, 1010);
        CHECK(ready.size() == 1);
        CHECK(ready[0].kind == (state == RowState::Unmapped ? HintKind::Unmapped : state == RowState::NotInUse ? HintKind::Full : HintKind::Connected));
    }
    for (bool gameplay : {false, true}) {
        ControllerHints hints; auto pad = View(1, RowState::Recognised); pad.gameplay = gameplay;
        auto equivalent = pad; equivalent.id = 9; equivalent.slot = 3; equivalent.identity.guid = "bluetooth";
        pad.identity.serial = equivalent.identity.serial = "serial";
        hints.Update({pad}, false, HintContext::HiddenMainMenu, 0);
        hints.Update({}, false, HintContext::HiddenMainMenu, 100);
        hints.Adopted(equivalent.identity, HintContext::HiddenMainMenu, 1098);
        CHECK(hints.Update({equivalent}, false, HintContext::HiddenMainMenu, 1099).empty());
        CHECK(hints.Update({equivalent}, false, HintContext::HiddenMainMenu, 2200).empty());
    }
    auto pad = View(1, RowState::Recognised); pad.gameplay = true;
    ControllerHints hints; hints.Update({pad}, false, HintContext::HiddenMainMenu, 0);
    hints.Update({}, false, HintContext::HiddenMainMenu, 100);
    CHECK(hints.Update({}, false, HintContext::HiddenMainMenu, 1100)[0].kind == HintKind::Disconnected);
    hints.Update({pad}, false, HintContext::HiddenMainMenu, 1200);
    CHECK(hints.Update({pad}, false, HintContext::HiddenMainMenu, 2200)[0].kind == HintKind::Reconnected);
    hints = {}; hints.Update({}, true, HintContext::OpenMainMenu, 0); hints.Update({pad}, true, HintContext::OpenMainMenu, 10);
    CHECK(hints.Update({pad}, true, HintContext::OpenMainMenu, 1010)[0].kind == HintKind::Keyboard);
    hints = {}; hints.Update({}, false, HintContext::OpenMainMenu, 0); hints.Update({pad}, false, HintContext::OpenMainMenu, 10);
    hints.Adopted(pad.identity, HintContext::OpenMainMenu, 20);
    CHECK(hints.Update({pad}, false, HintContext::OpenMainMenu, 1010)[0].kind == HintKind::Adopted);
    hints = {}; hints.Update({}, false, HintContext::Room, 0); hints.Update({pad}, false, HintContext::Room, 10);
    CHECK(hints.Update({pad}, false, HintContext::Room, 1010).empty());
    CHECK(hints.Update({pad}, false, HintContext::HiddenMainMenu, 2010).empty());
    hints = {}; hints.Update({}, false, HintContext::Room, 0); hints.Update({pad}, false, HintContext::Room, 10);
    CHECK(hints.Update({pad}, false, HintContext::HiddenMainMenu, 500).empty());
    CHECK(hints.Update({pad}, false, HintContext::HiddenMainMenu, 1010).empty());
    hints = {}; hints.Update({}, true, HintContext::OpenMainMenu, 0); hints.Update({pad}, true, HintContext::OpenMainMenu, 10);
    hints.Selected(pad.identity);
    CHECK(hints.Update({pad}, false, HintContext::OpenMainMenu, 1010).empty());
    hints = {}; pad.state = RowState::NotInUse; hints.Update({pad}, false, HintContext::OpenMainMenu, 0);
    pad.state = RowState::Recognised; hints.Update({pad}, true, HintContext::OpenMainMenu, 10);
    CHECK(hints.Update({pad}, true, HintContext::OpenMainMenu, 1010)[0].kind == HintKind::Connected);
}
static void InventoryProjection() {
    ControllerInventory inventory;
    PadFrame ps; ps.pad.id = 44; ps.pad.slot = 0; ps.pad.status = Status::Slotted;
    ps.pad.type = Type::PS5; ps.pad.identity = {"guid", "serial", "path", "DualSense"};
    ps.state.buttons[static_cast<unsigned>(Button::South)] = true;
    input::Device gameplay; gameplay.type = input::PadXInput; gameplay.identity = ps.pad.identity;
    auto pads = inventory.Build(Layer::Active, {ps}, {}, {}, gameplay, ps.pad.identity);
    CHECK(pads.size() == 1 && pads[0].id == 1 && pads[0].gameplay && pads[0].picture == "controller_playstation5");
    CHECK(pads[0].brand == PromptBrand(Brand::PlayStation, PlayStationArt::PS5) && pads[0].held == std::vector<std::string>({"A"}));
    pads = inventory.Build(Layer::Active, {}, {}, {}, gameplay, ps.pad.identity);
    CHECK(pads.size() == 1 && pads[0].id == 1 && pads[0].state == RowState::Absent && pads[0].picture == "controller_disconnected");
    ps.pad.id = 99; ps.pad.slot = 2; ps.pad.identity.guid = "bluetooth";
    pads = inventory.Build(Layer::Active, {ps}, {}, {}, gameplay, gameplay.identity);
    CHECK(pads.size() == 1 && pads[0].id == 1 && pads[0].slot == 2);
    PadFrame steam = ps; steam.pad.id = 100;
    steam.pad.identity = {"0300fa67de280000ff11000000007801", "", "steam", "Virtual"}; ps.pad.steamPad = 100;
    pads = inventory.Build(Layer::Active, {ps, steam}, {}, {}, {}, {});
    CHECK(pads.size() == 1 && pads[0].name == "DualSense" && pads[0].state == RowState::Steam);
    // The 2026-10-13 pad: Steam's GUID with the physical Xbox 360 pad's VID:PID.
    PadFrame xbox; xbox.pad.id = 102; xbox.pad.slot = 0; xbox.pad.status = Status::Slotted; xbox.pad.mapped = xbox.pad.sdlMapping = true;
    xbox.pad.vid = 0x045E; xbox.pad.pid = 0x028E; xbox.pad.type = Type::Xbox360;
    xbox.pad.identity = {"0300fa67de280000ff11000000007801", "", "steam-xbox", "Xbox 360 controller"};
    pads = inventory.Build(Layer::Active, {xbox}, {}, {}, {}, {});
    CHECK(pads.size() == 1 && pads[0].state == RowState::Steam && pads[0].picture == "controller_xbox360");
    CHECK(pads[0].brand == PromptBrand{Brand::Xbox} && pads[0].name == "Xbox 360 controller");
    auto row = ControllerRow(pads[0], true, true);
    CHECK(row.value == "Steam Input" && !row.choices[2].enabled && row.choices[2].detail == loc::T("controllers.choice_setup_steam"));
    xbox.pad.type = Type::Unknown;
    pads = inventory.Build(Layer::Active, {xbox}, {}, {}, {}, {});
    CHECK(pads[0].picture == "controller_steam" && pads[0].name.empty());
    // After an Ember setup a pad SDL knew keeps its brand and picture; one only Ember maps is a stick.
    PadFrame setUp; setUp.pad.id = 103; setUp.pad.slot = 0; setUp.pad.status = Status::Slotted; setUp.pad.type = Type::Xbox360;
    setUp.pad.identity = {"030000005e0400008e02000000007801", "", "xbox", "Xbox pad"};
    setUp.pad.mapped = setUp.pad.hasEmberMapping = setUp.pad.sdlMapping = true;
    pads = inventory.Build(Layer::Active, {setUp}, {}, {}, {}, {});
    CHECK(pads[0].brand == PromptBrand{Brand::Xbox} && pads[0].picture == "controller_xbox360" && pads[0].customMapping);
    setUp.pad.sdlMapping = false; setUp.pad.type = Type::Standard;
    pads = inventory.Build(Layer::Active, {setUp}, {}, {}, {}, {});
    CHECK(pads[0].brand == PromptBrand{Brand::Stick} && pads[0].picture == "generic_joystick_red" && pads[0].customMapping);
    input::Device game; game.type = input::PadDirectInput; game.index = 7; game.name = "Native stick"; game.connected = true;
    pads = inventory.Build(Layer::Failed, {}, {game}, {}, game, {});
    CHECK(pads.size() == 1 && pads[0].state == RowState::GameRead && pads[0].gameplay && pads[0].picture == "controller_generic");
    PadFrame raw; raw.pad.id = 101; raw.pad.status = Status::Unmapped; raw.pad.vid = 4; raw.pad.pid = 6;
    raw.pad.identity = {"raw", "", "PATH", "SDL stick"};
    pads = inventory.Build(Layer::Active, {raw}, {game}, {{7, "Native stick", "SDL stick", "path"}}, game, {});
    CHECK(pads.size() == 1 && pads[0].directInputIndex == 7 && pads[0].gameplay && pads[0].name == "SDL stick");
}
int main() { Rows(); LayoutEntryStates(); Held(); Hints(); InventoryProjection(); }
