#include "../common/GamepadBrand.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "../ui/ButtonPromptSetting.hxx"
#include "test_support.hxx"
#include <cstring>
using namespace sf4e;
using namespace sf4e::gamepads;
static bool Same(const char* a, const char* b) { return std::strcmp(a,b)==0; }
static constexpr PromptBrand brands[] = {
    {Brand::Xbox}, {Brand::PlayStation,PlayStationArt::PS3}, {Brand::PlayStation,PlayStationArt::PS4},
    {Brand::PlayStation,PlayStationArt::PS5}, {Brand::Nintendo}, {Brand::Stick}, {Brand::Generic}
};
static void Detection() {
    const struct { Type type; PromptBrand want; } cases[] = {
        {Type::Unknown,{}}, {Type::Standard,{}}, {Type::Xbox360,{Brand::Xbox}}, {Type::XboxOne,{Brand::Xbox}},
        {Type::PS3,{Brand::PlayStation,PlayStationArt::PS3}}, {Type::PS4,{Brand::PlayStation,PlayStationArt::PS4}},
        {Type::PS5,{Brand::PlayStation,PlayStationArt::PS5}}, {Type::SwitchPro,{Brand::Nintendo}},
        {Type::JoyConLeft,{Brand::Nintendo}}, {Type::JoyConRight,{Brand::Nintendo}},
        {Type::JoyConPair,{Brand::Nintendo}}, {Type::GameCube,{}}, {static_cast<Type>(999),{}}
    };
    for (const auto& c : cases) for (bool steam : {false,true}) {
        Pad pad; pad.type=c.type; pad.identity.guid=steam?"0300fa67de280000ff11000000007801":"";
        CHECK(DetectBrand(pad)==c.want);
        CHECK(GameplayBrand(input::PadXInput,&pad)==c.want);
        CHECK(GameplayBrand(input::PadDirectInput,&pad)==PromptBrand{});
        // A pad SDL maps keeps its brand after an Ember setup; one only Ember maps is a stick.
        pad.hasEmberMapping=true; pad.sdlMapping=true;
        CHECK(DetectBrand(pad)==c.want && !SetUpOnlyByEmber(pad));
        pad.sdlMapping=false;
        CHECK(DetectBrand(pad)==PromptBrand{Brand::Stick} && SetUpOnlyByEmber(pad));
    }
    CHECK(GameplayBrand(input::PadXInput)==PromptBrand{Brand::Xbox});
    CHECK(GameplayBrand(input::PadDirectInput)==PromptBrand{});
    CHECK(GameplayBrand(input::PadKeyboard)==PromptBrand{});
    // P4 supplies mappings; classification already makes the per-pad input explicit.
    Pad mapped; mapped.id=1; mapped.mapped=true; mapped.identity.guid="wizard";
    std::vector<Pad> pads{mapped}; Classify(pads,{}); CHECK(!pads[0].hasEmberMapping);
    Classify(pads,{"wizard"}); CHECK(pads[0].hasEmberMapping);
}
static void Resolution() {
    for (auto brand : brands) for (auto setting : {ButtonPrompts::Automatic,ButtonPrompts::Xbox,ButtonPrompts::PlayStation,ButtonPrompts::Nintendo}) {
        const auto expected=setting==ButtonPrompts::Automatic?brand:setting==ButtonPrompts::Xbox?PromptBrand{Brand::Xbox}:
            setting==ButtonPrompts::PlayStation?PromptBrand{Brand::PlayStation,PlayStationArt::PS4}:PromptBrand{Brand::Nintendo};
        CHECK(ResolvePrompts(setting,input::PadXInput,brand)==expected);
        for (int device : {input::PadKeyboard,input::PadDirectInput}) {
            const auto resolved=ResolvePrompts(setting,device,brand);
            CHECK(resolved==PromptBrand{});
            CHECK(Same(ui::BrandGlyph("LP",resolved).asset,"generic_button_circle_fill"));
            CHECK(Same(ui::BrandGlyph("LP",resolved).name,"LP"));
            CHECK(Same(ui::BrandGlyph("LK",resolved).name,"LK"));
        }
    }
    // Opt-out or failed layer still gives an XInput controller Xbox by default.
    CHECK(ResolvePrompts(ButtonPrompts::Automatic,input::PadXInput,GameplayBrand(input::PadXInput))==PromptBrand{Brand::Xbox});
    CHECK(Valid(ButtonPrompts::Automatic)&&Valid(ButtonPrompts::Nintendo));
    CHECK(!Valid(static_cast<ButtonPrompts>(-1))&&!Valid(static_cast<ButtonPrompts>(4)));
    CHECK(ui::ButtonPromptValue(ButtonPrompts::Automatic)=="Automatic");
    CHECK(ui::ButtonPromptValue(ButtonPrompts::Xbox)=="Xbox");
    CHECK(ui::ButtonPromptValue(ButtonPrompts::PlayStation)=="PlayStation");
    CHECK(ui::ButtonPromptValue(ButtonPrompts::Nintendo)=="Nintendo");
}
static void Glyphs() {
    const struct { const char* id; const char* xbox; const char* ps; const char* nintendo; const char* stick; const char* name; } cases[] = {
        {"A","xbox_button_color_a","playstation_button_color_cross","switch_button_b","generic_button_circle_fill","LK"},
        {"B","xbox_button_color_b","playstation_button_color_circle","switch_button_a","generic_button_circle_fill","MK"},
        {"X","xbox_button_color_x","playstation_button_color_square","switch_button_y","generic_button_circle_fill","LP"},
        {"Y","xbox_button_color_y","playstation_button_color_triangle","switch_button_x","generic_button_circle_fill","MP"},
        {"LB","xbox_lb","playstation_trigger_l1","switch_button_l","generic_button_circle_fill","3P"},
        {"RB","xbox_rb","playstation_trigger_r1","switch_button_r","generic_button_circle_fill","HP"},
        {"LT","xbox_lt","playstation_trigger_l2","switch_button_zl","generic_button_circle_fill","3K"},
        {"RT","xbox_rt","playstation_trigger_r2","switch_button_zr","generic_button_circle_fill","HK"},
        {"View","xbox_button_back","playstation4_button_share","switch_button_minus","generic_button_circle_fill","Select"},
        {"Start","xbox_button_start","playstation4_button_options","switch_button_plus","generic_button_circle_fill","Start"},
        {"dpad","xbox_dpad","playstation_dpad","switch_dpad","generic_joystick",""},
        {"horizontal","xbox_dpad_horizontal","playstation_dpad_horizontal","switch_dpad_horizontal","generic_joystick_horizontal",""},
        {"up","xbox_dpad_up","playstation_dpad_up","switch_dpad_up","generic_stick_up",""},
        {"down","xbox_dpad_down","playstation_dpad_down","switch_dpad_down","generic_stick_down",""},
        {"left","xbox_dpad_left","playstation_dpad_left","switch_dpad_left","generic_stick_left",""},
        {"right","xbox_dpad_right","playstation_dpad_right","switch_dpad_right","generic_stick_right",""}
    };
    for (const auto& c : cases) for (auto brand : brands) {
        const char* asset=brand.brand==Brand::Stick?c.stick:brand.brand==Brand::Nintendo?c.nintendo:
            brand.brand==Brand::PlayStation?c.ps:c.xbox;
        if (brand.brand==Brand::PlayStation && Same(c.id,"View"))
            asset=brand.playStation==PlayStationArt::PS3?"playstation3_button_select":brand.playStation==PlayStationArt::PS5?"playstation5_button_create":c.ps;
        if (brand.brand==Brand::PlayStation && Same(c.id,"Start"))
            asset=brand.playStation==PlayStationArt::PS3?"playstation3_button_start":brand.playStation==PlayStationArt::PS5?"playstation5_button_options":c.ps;
        const auto result=ui::BrandGlyph(c.id,brand);
        CHECK(Same(result.asset,asset));
        CHECK(Same(ui::PromptAsset(c.id,brand),asset));
        CHECK(Same(result.name,brand.brand==Brand::Stick?c.name:""));
        const auto label=ui::PromptLabel(c.id,"Action",brand);
        CHECK(label==(brand.brand==Brand::Stick&&*c.name?std::string(c.name)+" Action":"Action"));
    }
    for (auto brand : brands) {
        for (const auto& key : {"Enter","Ctrl+Enter","Esc","arrows","keys-horizontal"})
            CHECK(Same(ui::PromptAsset(key,brand),ui::PromptAsset(key)));
        CHECK(ui::PromptLabel("F1","Pause",brand)=="Pause");
        CHECK(ui::PromptLabel("Ctrl+Enter","Ready",brand)=="Ctrl+Enter Ready");
        CHECK(Same(ui::PhysicalGlyph(input::PadXInput,input::xinput::A,"LP"),"A"));
    }
    CHECK(Same(ui::PhysicalGlyph(input::PadDirectInput,input::xinput::A,"LP"),"LP"));
}
static void RelayoutNames() {
    auto layout = DefaultLayout; Assign(layout, 5, 0x10); SetMask(layout, 0, 0);
    const auto names = NamesOf(layout);
    CHECK(Same(ui::BrandGlyph("RB", {Brand::Stick}, &names).name, "LP"));
    CHECK(Same(ui::BrandGlyph("X", {Brand::Stick}, &names).name, "HP"));
    CHECK(ui::PromptLabel("A", "Select", {Brand::Stick}, &names) == "Select");
    CHECK(Same(ui::BrandGlyph("LS", {Brand::PlayStation}).asset, "playstation_button_l3"));
    CHECK(Same(ui::BrandGlyph("RS", {Brand::Nintendo}).asset, "switch_stick_r_press"));
}
static void Names() {
    const PositionNames expected[]={{"Back","Start"},{"Select","Start"},{"Share","Options"},{"Create","Options"},
        {"Minus","Plus"},{"Select","Start"},{"Back","Start"}};
    for (unsigned i=0;i<std::size(brands);++i) {
        const auto names=ButtonPositionNames(input::PadXInput,brands[i]);
        CHECK(Same(names.view,expected[i].view)&&Same(names.start,expected[i].start));
        const auto direct=ButtonPositionNames(input::PadDirectInput,brands[i]);
        CHECK(Same(direct.view,"Select")&&Same(direct.start,"Start"));
    }
}
// Speech names a button as the player's pad or stick prints it.
static void SpokenNames() {
    const auto names = NamesOf(DefaultLayout);
    CHECK(ui::SpokenButton("A", {Brand::Xbox}, &names) == "A" && ui::SpokenButton("LB", {Brand::Xbox}, &names) == "LB");
    CHECK(ui::SpokenButton("A", {Brand::PlayStation, PlayStationArt::PS4}, &names) == "Cross");
    CHECK(ui::SpokenButton("RB", {Brand::PlayStation, PlayStationArt::PS4}, &names) == "R1");
    CHECK(ui::SpokenButton("A", {Brand::Nintendo}, &names) == "B" && ui::SpokenButton("LT", {Brand::Nintendo}, &names) == "ZL");
    CHECK(ui::SpokenButton("A", {Brand::Stick}, &names) == "LK");
    CHECK(ui::SpokenButton("Enter", {}, &names) == "Enter" && ui::SpokenButton("Esc", {}, &names) == "Escape" && ui::SpokenButton("F1", {}, &names) == "F1");
}
int main() { Detection(); Resolution(); Glyphs(); Names(); RelayoutNames(); SpokenNames(); }
