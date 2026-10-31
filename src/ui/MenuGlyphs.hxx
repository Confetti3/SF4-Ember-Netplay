#pragma once
#include "Theme.hxx"
#include "SelectionArt.hxx"
#include "MenuProbes.hxx"
#include "../common/Localization.hxx"
#include "../common/ButtonPrompts.hxx"
#include "../common/ButtonLayout.hxx"
#include "../common/MenuButtons.hxx"
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// Native XInput normalization at 006D8415..006D8466. DirectInput device shape
// cannot be inferred from its binding index; never pretend it is an Xbox pad.
inline const char* PhysicalGlyph(int type,unsigned mask,const char* fallback) {
    const char* glyph=type==input::PadXInput?input::XInputButtonName(mask):nullptr;
    return glyph?glyph:fallback;
}
gamepads::PromptBrand MenuPromptBrand();
// A shell's unsaved value takes effect before the normal autosave deadline.
const gamepads::FightNames* MenuPromptNames();
void SetMenuPromptBrand(gamepads::PromptBrand brand, const gamepads::FightNames& names = gamepads::NamesOf(gamepads::DefaultLayout));
struct BrandedGlyph { const char* asset; const char* name; };
inline BrandedGlyph BrandGlyph(const char* id, gamepads::PromptBrand brand, const gamepads::FightNames* names = nullptr) {
    struct Position { const char* id; const char* xbox; const char* playStation; const char* nintendo; const char* stick; const char* fight; };
    static constexpr Position positions[] = {
        {"A", "xbox_button_color_a", "playstation_button_color_cross", "switch_button_b", "generic_button_circle_fill", "LK"},
        {"B", "xbox_button_color_b", "playstation_button_color_circle", "switch_button_a", "generic_button_circle_fill", "MK"},
        {"X", "xbox_button_color_x", "playstation_button_color_square", "switch_button_y", "generic_button_circle_fill", "LP"},
        {"Y", "xbox_button_color_y", "playstation_button_color_triangle", "switch_button_x", "generic_button_circle_fill", "MP"},
        {"LB", "xbox_lb", "playstation_trigger_l1", "switch_button_l", "generic_button_circle_fill", "3P"},
        {"RB", "xbox_rb", "playstation_trigger_r1", "switch_button_r", "generic_button_circle_fill", "HP"},
        {"LT", "xbox_lt", "playstation_trigger_l2", "switch_button_zl", "generic_button_circle_fill", "3K"},
        {"RT", "xbox_rt", "playstation_trigger_r2", "switch_button_zr", "generic_button_circle_fill", "HK"},
        {"LS", "xbox_ls", "playstation_button_l3", "switch_stick_l_press", "generic_button_circle_fill", ""},
        {"RS", "xbox_rs", "playstation_button_r3", "switch_stick_r_press", "generic_button_circle_fill", ""},
        {"View", "xbox_button_back", "playstation4_button_share", "switch_button_minus", "generic_button_circle_fill", "Select"},
        {"Start", "xbox_button_start", "playstation4_button_options", "switch_button_plus", "generic_button_circle_fill", "Start"},
        {"dpad", "xbox_dpad", "playstation_dpad", "switch_dpad", "generic_joystick", ""},
        {"horizontal", "xbox_dpad_horizontal", "playstation_dpad_horizontal", "switch_dpad_horizontal", "generic_joystick_horizontal", ""},
        {"up", "xbox_dpad_up", "playstation_dpad_up", "switch_dpad_up", "generic_stick_up", ""},
        {"down", "xbox_dpad_down", "playstation_dpad_down", "switch_dpad_down", "generic_stick_down", ""},
        {"left", "xbox_dpad_left", "playstation_dpad_left", "switch_dpad_left", "generic_stick_left", ""},
        {"right", "xbox_dpad_right", "playstation_dpad_right", "switch_dpad_right", "generic_stick_right", ""},
    };
    for (const auto& position : positions) if (!std::strcmp(id, position.id)) {
        switch (brand.brand) {
        case gamepads::Brand::Stick:
            if (names) for (unsigned i = 0; i < gamepads::LayoutGlyphs.size(); ++i)
                if (!std::strcmp(id, gamepads::LayoutGlyphs[i])) return {position.stick, (*names)[i].c_str()};
            return {position.stick, position.fight};
        case gamepads::Brand::PlayStation:
            if (!std::strcmp(id, "View")) return {
                brand.playStation == gamepads::PlayStationArt::PS3 ? "playstation3_button_select" :
                brand.playStation == gamepads::PlayStationArt::PS5 ? "playstation5_button_create" : "playstation4_button_share", ""};
            if (!std::strcmp(id, "Start")) return {
                brand.playStation == gamepads::PlayStationArt::PS3 ? "playstation3_button_start" :
                brand.playStation == gamepads::PlayStationArt::PS5 ? "playstation5_button_options" : "playstation4_button_options", ""};
            return {position.playStation, ""};
        case gamepads::Brand::Nintendo: return {position.nintendo, ""};
        default: return {position.xbox, ""};
        }
    }
    if (!std::strcmp(id, "Enter") || !std::strcmp(id, "Ctrl+Enter")) return {"keyboard_enter", ""};
    if (!std::strcmp(id, "Esc")) return {"keyboard_escape", ""};
    if (!std::strcmp(id, "arrows")) return {"keyboard_arrows_all", ""};
    if (!std::strcmp(id, "keys-horizontal")) return {"keyboard_arrows_horizontal", ""};
    if (!std::strcmp(id, "generic_stick_left") || !std::strcmp(id, "generic_stick_right") || !std::strcmp(id, "generic_button_circle_fill")) return {id, ""};
    return {"generic_button_circle_fill", id};
}
inline const char* PromptAsset(const char* glyph, gamepads::PromptBrand brand = {}) {
    return BrandGlyph(glyph, brand).asset;
}
// The keyboard keys the legend draws as a key cap with their name on it; the
// prompt art has no letter keys.
inline bool KeyCapGlyph(const char* glyph) {
    // The function keys F1 to F12 too, which the replay controls use.
    const bool function=glyph[0]=='F'&&glyph[1]>='1'&&glyph[1]<='9'&&(!glyph[2]||(glyph[1]=='1'&&glyph[2]>='0'&&glyph[2]<='2'&&!glyph[3]));
    return function||!std::strcmp(glyph,"F")||!std::strcmp(glyph,"T")||!std::strcmp(glyph,"C")||!std::strcmp(glyph,"Del")||!std::strcmp(glyph,"Bksp")||!std::strcmp(glyph,"Space")||!std::strcmp(glyph,"PgUp")||!std::strcmp(glyph,"PgDn");
}
inline std::string PromptLabel(const char* glyph,const std::string& label,gamepads::PromptBrand brand,const gamepads::FightNames* names=MenuPromptNames()) {
    if(!std::strcmp(glyph,"Ctrl+Enter"))return "Ctrl+Enter "+label;
    const auto translated=BrandGlyph(glyph,brand,names);
    return !KeyCapGlyph(glyph)&&*translated.name?std::string(translated.name)+" "+label:label;
}
// One prompt in a size-by-size square at `at`: a key cap with its name, the
// prompt art, or, with neither, the glyph's name. `s` is the caller's scale; `keyFont`,
// when given, sizes a key cap's name.
inline void DrawPromptGlyph(ImDrawList* d,const char* glyph,ImVec2 at,float size,SelectionArt* art,float s,float keyFont=0,gamepads::PromptBrand brand=MenuPromptBrand(),float alpha=1,const gamepads::FightNames* names=MenuPromptNames()) {
    auto textColor=ImGui::ColorConvertU32ToFloat4(palette::Ivory);textColor.w*=alpha;
    const auto ivory=ImGui::GetColorU32(textColor);
    if(KeyCapGlyph(glyph)){
        const ImVec2 capTop(at.x+2*s,at.y+3*s),capBottom(at.x+size-2*s,at.y+size-3*s);
        d->AddRectFilled(capTop,capBottom,IM_COL32(40,38,36,static_cast<int>(235*alpha)),5*s);
        d->AddRect(capTop,capBottom,ivory,5*s,0,1.5f*s);
        // 12 and 16 at the legend's 32-pixel prompts, and in step at other sizes.
        if(keyFont<=0)keyFont=(std::strlen(glyph)>1?12:16)*size/32;
        // A long legend ("PgDn") shrinks to fit inside the cap instead of running past it.
        const float room=capBottom.x-capTop.x-4*s,wide=ImGui::GetFont()->CalcTextSizeA(keyFont,FLT_MAX,0,glyph).x;
        if(wide>room&&wide>0)keyFont*=room/wide;
        const ImVec2 text=ImGui::GetFont()->CalcTextSizeA(keyFont,FLT_MAX,0,glyph);
        d->AddText(ImGui::GetFont(),keyFont,ImVec2((capTop.x+capBottom.x-text.x)*.5f,(capTop.y+capBottom.y-text.y)*.5f),ivory,glyph);
        return;
    }
    const auto icon=art?art->InputPrompt(PromptAsset(glyph,brand)):SelectionImage{};
    const auto tint=ImGui::GetColorU32(ImVec4(1,1,1,alpha));
    if(icon.texture){
        d->AddImage(icon.texture,at,ImVec2(at.x+size,at.y+size),icon.uvMin,icon.uvMax,tint);
        const auto name=BrandGlyph(glyph,brand,names).name;
        if(brand.brand==gamepads::Brand::Stick&&*name){
            const float base=12*size/32;
            const float measured=ImGui::GetFont()->CalcTextSizeA(base,FLT_MAX,0,name).x;
            const float font=base*(std::min)(1.f,(size-8*size/32)/(std::max)(1.f,measured));
            const auto text=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,name);
            d->AddText(ImGui::GetFont(),font,ImVec2(at.x+(size-text.x)*.5f,at.y+(size-text.y)*.5f),ImGui::GetColorU32(ImVec4(.12f,.11f,.1f,alpha)),name);
        }
    }
    else {
        const char* name=BrandGlyph(glyph,brand,names).name;
        d->AddText(ImGui::GetFont(),12*size/32,ImVec2(at.x,at.y+8*size/32),ivory,*name?name:glyph);
    }
}
// A prompt's name as speech says it: a stick's own button name, else the last
// word of its art, so a pad's button is named as its brand prints it
// ("playstation_button_color_cross" is Cross, "xbox_lb" is LB, "keyboard_enter" Enter).
inline std::string SpokenButton(const char* glyph,gamepads::PromptBrand brand=MenuPromptBrand(),const gamepads::FightNames* names=MenuPromptNames()) {
    const auto found=BrandGlyph(glyph,brand,names);
    if(*found.name)return found.name;
    const char* last=std::strrchr(found.asset,'_');
    std::string word=last?last+1:found.asset;
    for(std::size_t i=0;i<word.size();++i)if(i==0||word.size()<=2)word[i]=static_cast<char>(std::toupper(static_cast<unsigned char>(word[i])));
    return word;
}
// A screen's own button, shown after the standard ones.
// A hint with a press (a MenuInput button) is also a button that presses it.
struct LegendHint { const char* glyph; std::string label; unsigned press = 0; };
} }
