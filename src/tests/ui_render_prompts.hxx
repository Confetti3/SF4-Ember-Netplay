#pragma once
#include "../ui/ApplicationShell.hxx"
#include "../ui/TrainingPanel.hxx"
namespace {
template<class Draw,class Page>
void ShootButtonPrompts(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,int& mode,const Draw& draw,const Page& page) {
    using namespace sf4e; using namespace ui; using namespace gamepads;
    const auto savedDevice=view.inputDevice;
    const auto savedBrand=view.gameplayBrand;
    const auto savedPrompts=view.preferences.buttonPrompts;
    const struct { PromptBrand brand; const char* shot; } scenarios[]={
        {{Brand::Xbox},"prompts-xbox"},
        {{Brand::PlayStation,PlayStationArt::PS3},"prompts-ps3"},
        {{Brand::PlayStation,PlayStationArt::PS4},"prompts-ps4"},
        {{Brand::PlayStation,PlayStationArt::PS5},"prompts-ps5"},
        {{Brand::Nintendo},"prompts-nintendo"},
        {{Brand::Stick},"prompts-stick"}
    };
    view.inputDevice.type=input::PadXInput;
    view.preferences.buttonPrompts=ButtonPrompts::Automatic;
    for(const auto& scenario:scenarios) {
        view.gameplayBrand=view.promptBrand=scenario.brand;
        SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B,"LP","LK",scenario.brand);
        NoteMenuDevice(0);NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);
        page("player");
        for(int i=0;i<12&&shell.Navigation().Focus()!="button-prompts";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
        Require(shell.Navigation().Focus()=="button-prompts","Button prompts row unreachable");draw(scenario.shot);
        Require(MenuPromptBrand()==scenario.brand,"Shell lost its detected prompt brand");
        if(scenario.brand.brand==Brand::Nintendo)
            Require(!std::strcmp(PromptAsset("A",MenuPromptBrand()),"switch_button_b"),"Nintendo south did not show printed B");
        if(scenario.brand.brand==Brand::Stick) {
            Require(PromptLabel("A",loc::T("menu.select"),MenuPromptBrand())==std::string("LK ")+loc::T("menu.select"),"Stick confirm lacks LK");
            Require(PromptLabel("B",loc::T("common.back"),MenuPromptBrand())==std::string("MK ")+loc::T("common.back"),"Stick back lacks MK");
        }
        mode=2;draw((std::string(scenario.shot)+"-training").c_str());
        const auto names=ButtonPositionNames(input::PadXInput,scenario.brand);
        const auto& strip=LastTrainingStrip();
        Require(std::find(strip.pieces.begin(),strip.pieces.end(),names.view)!=strip.pieces.end(),"Training strip lost brand's View name");
        // An Xbox-type pad opens the controls with R3 alone (TrainingPad.hxx), so the strip names R3, not Start.
        Require(std::find(strip.pieces.begin(),strip.pieces.end(),"R3")!=strip.pieces.end(),"Training strip lost R3, the pad's way to the controls");
        mode=0;
    }
    SetMenuGlyphs(input::PadKeyboard,0,0);draw("prompts-keyboard");
    Require(KeyboardPrompts(),"Keyboard prompts were overridden by brand");
    view.inputDevice=savedDevice;view.gameplayBrand=savedBrand;view.promptBrand=savedBrand;view.preferences.buttonPrompts=savedPrompts;
    SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B);page("player");
    for(int i=0;i<12&&shell.Navigation().Focus()!="name";++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
    Require(shell.Navigation().Focus()=="name","Prompt scenarios did not restore Player focus");page("home");
}
template<class Draw,class Page>
void ShootRoomPromptBrands(sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    using namespace sf4e;using namespace ui;using namespace gamepads;
    const auto savedDevice=view.inputDevice;
    const auto savedBrand=view.gameplayBrand;
    for(const auto brand:{PromptBrand{Brand::PlayStation,PlayStationArt::PS5},PromptBrand{Brand::Nintendo},PromptBrand{Brand::Stick}}) {
        view.inputDevice.type=input::PadXInput;view.gameplayBrand=view.promptBrand=brand;
        SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B,"LP","LK",brand);
        NoteMenuDevice(0);NoteMenuDevice(MenuInput::Down);NoteMenuDevice(0);
        page("room");
        draw(brand.brand==Brand::PlayStation?"room-prompts-ps5":brand.brand==Brand::Nintendo?"room-prompts-nintendo":"room-prompts-stick");
    }
    view.inputDevice=savedDevice;view.gameplayBrand=view.promptBrand=savedBrand;
    SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B);
}

}
