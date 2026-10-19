// Which physical buttons confirm and go back in Ember's menus: the player's
// binding, else the game's menu table (IDA notes in CONTROLLER_LAYER_UX 11.14).
#include "../common/MenuButtons.hxx"
#include "../common/ButtonLayoutJson.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "test_support.hxx"
#include <array>
#include <cstring>

using namespace sf4e;
using namespace sf4e::input;
namespace {
using Table = std::array<std::uint32_t, 32>;
bool Same(const char* a, const char* b) { return a && b && !std::strcmp(a, b); }
// The game's XInput menu default (0xA5D0F8) and generic DirectInput menu default (0xA5D1F8).
Table XInputMenu() {
    Table t{}; t[0] = 1; t[1] = 2; t[2] = 4; t[3] = 8; t[8] = 0x2000; t[9] = 0x1000;
    t[16] = 0x20; t[17] = 0x80; t[18] = 0x40; t[19] = 0x10; t[20] = 0x100; t[21] = 0x400; t[22] = 0x200; t[23] = 0x800;
    return t;
}
Table StickMenu() {
    Table t{}; t[16] = 0x20; t[17] = 0x80; t[18] = 0x40; t[19] = 0x10; t[20] = 0x100; t[21] = 0x400; t[22] = 0x200; t[23] = 0x800;
    t[24] = 0x2000; t[25] = 0x1000; return t;
}
// The glyph a menu shows is the name of the button that acts.
void GlyphMatchesAction(int type, const MenuButtons& buttons) {
    CHECK(Same(ui::PhysicalGlyph(type, buttons.confirm, buttons.confirmGlyph), buttons.confirmGlyph));
    CHECK(Same(ui::PhysicalGlyph(type, buttons.back, buttons.backGlyph), buttons.backGlyph));
    CHECK(ui::ControllerButtons(0, type, buttons.confirm, buttons) == ui::ControllerSample::Confirm);
    CHECK(ui::ControllerButtons(0, type, buttons.back, buttons) == ui::ControllerSample::Back);
    if (type == PadXInput) CHECK(Same(XInputButtonName(buttons.confirm), buttons.confirmGlyph) && Same(XInputButtonName(buttons.back), buttons.backGlyph));
}
void Resolver() {
    // An Xbox pad keeps A and B by default, whatever its fight table says.
    auto menu = XInputMenu(); Table fight{}; fight[18] = 0x10; fight[19] = 0x40;
    auto buttons = ResolveMenuButtons(PadXInput, menu.data(), fight.data());
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::B && Same(buttons.confirmGlyph, "A") && Same(buttons.backGlyph, "B"));
    GlyphMatchesAction(PadXInput, buttons);
    // LK and MK moved in the game's own button screen: the tables follow to X
    // and Y. MenuButtonsFor passes no tables for an XInput pad, only a stick's.
    menu[18] = 0x10; menu[19] = 0x40; menu[16] = 0x80; menu[17] = 0x20;
    buttons = ResolveMenuButtons(PadXInput, menu.data(), fight.data());
    CHECK(buttons.confirm == xinput::X && buttons.back == xinput::Y && Same(buttons.confirmGlyph, "X") && Same(buttons.backGlyph, "Y"));
    GlyphMatchesAction(PadXInput, buttons);
    CHECK(ui::ControllerButtons(0, PadXInput, xinput::A, buttons) == 0);
    // A stick confirms on its LK button and goes back on its MK button, by the
    // menu table; one moved to button 6 confirms there, named by its function.
    auto stick = StickMenu(); Table stickFight{};
    buttons = ResolveMenuButtons(PadDirectInput, stick.data(), stickFight.data());
    CHECK(buttons.confirm == 1u << 18 && buttons.back == 1u << 17 && Same(buttons.confirmGlyph, "LK") && Same(buttons.backGlyph, "MK"));
    stick[18] = 0x400; stick[21] = 0x40;
    buttons = ResolveMenuButtons(PadDirectInput, stick.data(), stickFight.data());
    CHECK(buttons.confirm == 1u << 21 && Same(buttons.confirmGlyph, "LK"));
    GlyphMatchesAction(PadDirectInput, buttons);
    CHECK(ui::ControllerButtons(0x40, PadDirectInput, 1u << 18, buttons) == 0);
    // No menu entry: the fight table's LK and MK. Directions never count, nor an empty entry.
    Table none{}; Table fightOnly{}; fightOnly[2] = 0x40; fightOnly[20] = 0x40; fightOnly[23] = 0x80;
    buttons = ResolveMenuButtons(PadDirectInput, none.data(), fightOnly.data());
    CHECK(buttons.confirm == 1u << 20 && buttons.back == 1u << 23);
    // No valid record: bits 18 and 17.
    buttons = ResolveMenuButtons(PadDirectInput, nullptr, nullptr);
    CHECK(buttons.confirm == 1u << 18 && buttons.back == 1u << 17 && Same(buttons.confirmGlyph, "LK"));
    buttons = ResolveMenuButtons(PadXInput, nullptr, nullptr);
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::B);
    // A zero function is the game's default, not "the first empty entry".
    buttons = ResolveMenuButtons(PadDirectInput, StickMenu().data(), none.data(), 0, 0);
    CHECK(buttons.confirm == 1u << 18 && buttons.back == 1u << 17);
    // A swapped System (decide 0x80, cancel 0x40) confirms on MK.
    buttons = ResolveMenuButtons(PadXInput, XInputMenu().data(), none.data(), 0x80, 0x40);
    CHECK(buttons.confirm == xinput::B && buttons.back == xinput::A);
    // The player's binding wins, each button on its own.
    buttons = ResolveMenuButtons(PadXInput, XInputMenu().data(), none.data(), MenuConfirmFunction, MenuBackFunction, xinput::RB, 0);
    CHECK(buttons.confirm == xinput::RB && buttons.back == xinput::B && Same(buttons.confirmGlyph, "RB"));
    GlyphMatchesAction(PadXInput, buttons);
    buttons = ResolveMenuButtons(PadXInput, XInputMenu().data(), none.data(), MenuConfirmFunction, MenuBackFunction, 0, xinput::LT);
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::LT && Same(buttons.backGlyph, "LT"));
    // A binding that lands on the other's game button takes it over; the two never share one.
    buttons = ResolveMenuButtons(PadXInput, XInputMenu().data(), none.data(), MenuConfirmFunction, MenuBackFunction, xinput::B, 0);
    CHECK(buttons.confirm == xinput::B && buttons.back == xinput::A);
    buttons = ResolveMenuButtons(PadXInput, XInputMenu().data(), none.data(), MenuConfirmFunction, MenuBackFunction, xinput::X, xinput::X);
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::B);
    // Room shortcuts lose only the button that confirms or goes back.
    MenuButtons onX; onX.confirm = xinput::X; onX.confirmGlyph = "X";
    CHECK(ui::ControllerButtons(0, PadXInput, xinput::X | xinput::Y | xinput::View, onX) ==
        (ui::ControllerSample::Confirm | ui::ControllerSample::Options | ui::ControllerSample::Chat));
}
alignas(4) std::array<BYTE, 0x100> nativeSystem{};
alignas(4) std::array<BYTE, 12 * 0x27c> nativeRecords{};
Dimps::Pad::System* SystemStub() { return reinterpret_cast<Dimps::Pad::System*>(nativeSystem.data()); }
Dimps::Pad::System* NoSystem() { return nullptr; }
std::uint32_t* RecordTable(int device, unsigned offset) { return reinterpret_cast<std::uint32_t*>(nativeRecords.data() + device * 0x27c + offset); }
// The adapter reads +0xCC/+0xD0 and the record's menu table (+0x1FC), and never writes.
void Adapter() {
    Dimps::Pad::System::staticMethods.GetSingleton = SystemStub;
    *reinterpret_cast<std::uint32_t*>(nativeSystem.data() + 0xcc) = 0x40;
    *reinterpret_cast<std::uint32_t*>(nativeSystem.data() + 0xd0) = 0x80;
    *reinterpret_cast<BYTE**>(nativeSystem.data() + 0xd4) = nativeRecords.data();
    *reinterpret_cast<BYTE**>(nativeSystem.data() + 0xd8) = nativeRecords.data() + nativeRecords.size();
    const auto records = Dimps::Pad::BindingRecords();
    CHECK(records.MenuTable(2) == RecordTable(2, 0x1fc) && records.FightTable(2) == RecordTable(2, 0x17c) && !records.MenuTable(12));
    const auto xinput = XInputMenu(); std::memcpy(RecordTable(1, 0x1fc), xinput.data(), sizeof(xinput));
    // A remapped stick: the game's screen moved LK and MK to buttons 5 and 6 in both tables.
    auto stick = StickMenu(); stick[18] = 0x10; stick[17] = 0x20; stick[20] = 0x40; stick[21] = 0x80;
    std::memcpy(RecordTable(6, 0x1fc), stick.data(), sizeof(stick));
    RecordTable(6, 0x17c)[20] = 0x40; RecordTable(6, 0x17c)[21] = 0x80;
    const auto before = nativeRecords;
    auto buttons = Dimps::Pad::MenuButtons(PadXInput, 1);
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::B);
    buttons = Dimps::Pad::MenuButtons(PadDirectInput, 6);
    CHECK(buttons.confirm == 1u << 20 && buttons.back == 1u << 21 && Same(buttons.confirmGlyph, "LK") && Same(buttons.backGlyph, "MK"));
    CHECK(ui::ControllerButtons(0, PadDirectInput, 1u << 20, buttons) == ui::ControllerSample::Confirm);
    CHECK(ui::ControllerButtons(0, PadDirectInput, 1u << 18, buttons) == 0);
    CHECK(nativeRecords == before);
    // An unknown device or a record past the vector's end: the game's generic default.
    *reinterpret_cast<BYTE**>(nativeSystem.data() + 0xd8) = nativeRecords.data() + 3 * 0x27c;
    buttons = Dimps::Pad::MenuButtons(PadDirectInput, 6);
    CHECK(buttons.confirm == 1u << 18 && buttons.back == 1u << 17);
    buttons = Dimps::Pad::MenuButtons(PadXInput, -1);
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::B);
    Dimps::Pad::System::staticMethods.GetSingleton = NoSystem;
    buttons = Dimps::Pad::MenuButtons(PadXInput, 1);
    CHECK(buttons.confirm == xinput::A && buttons.back == xinput::B);
}
void Storage() {
    using namespace sf4e::gamepads;
    CHECK(ValidMenuBinding({}) && ValidMenuBinding({2, 3}) && ValidMenuBinding({2, -1}) && ValidMenuBinding({-1, 1}));
    CHECK(!ValidMenuBinding({2, 2}) && !ValidMenuBinding({10, -1}) && !ValidMenuBinding({-2, 0}));
    CHECK(MenuBindingMask(2) == xinput::X && MenuBindingMask(-1) == 0 && MenuBindingMask(10) == 0);
    auto has = AllLayoutButtons; has[8] = false; CHECK(MenuBindingMask(8, has) == 0);
    StoredLayout layout{{"guid", "serial", "path", "Pad"}, DefaultLayout, 5, {2, 3}};
    const auto json = nlohmann::json{{"buttonLayouts", LayoutsToJson({layout})}};
    const auto read = ReadLayouts(json, [](unsigned, LayoutLoadError) {});
    CHECK(read.size() == 1 && read[0] == layout);
    // An old entry without the fields, or with an unusable binding, loads with the game's menu buttons.
    for (const auto& bad : {nlohmann::json(nullptr), nlohmann::json(4), nlohmann::json(11)}) {
        auto entry = json; entry["buttonLayouts"][0]["menuBack"] = bad;
        if (bad.is_null()) { entry["buttonLayouts"][0].erase("menuBack"); entry["buttonLayouts"][0].erase("menuConfirm"); }
        if (bad == 4) entry["buttonLayouts"][0]["menuConfirm"] = 4;
        const auto loaded = ReadLayouts(entry, [](unsigned, LayoutLoadError) {});
        CHECK(loaded.size() == 1 && loaded[0].buttons == DefaultLayout && loaded[0].menu == MenuBinding{});
    }
    netplay::PlayerPreferences preferences; preferences.buttonLayouts = {layout};
    CHECK(preferences.Valid());
    preferences.buttonLayouts[0].menu = {3, 3}; CHECK(!preferences.Valid());
}
}
int main() { Resolver(); Adapter(); Storage(); }
