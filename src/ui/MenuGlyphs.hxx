#pragma once
#include "Theme.hxx"
#include "SelectionArt.hxx"
#include "../common/Localization.hxx"
#include <cstring>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// Native XInput normalization at 006D8415..006D8466. DirectInput device shape
// cannot be inferred from its binding index; never pretend it is an Xbox pad.
inline const char* PhysicalGlyph(int type,unsigned mask,const char* fallback) {
    if(type!=3)return fallback;
    switch(mask){case 0x40000:return "A";case 0x20000:return "B";case 0x80000:return "X";case 0x10000:return "Y";
    case 0x100000:return "LB";case 0x200000:return "RB";case 0x400000:return "LT";case 0x800000:return "RT";default:return fallback;}
}
inline const char* PromptAsset(const char* glyph) {
    if(!std::strcmp(glyph,"A"))return "xbox_button_color_a";
    if(!std::strcmp(glyph,"B"))return "xbox_button_color_b";
    if(!std::strcmp(glyph,"X"))return "xbox_button_color_x";
    if(!std::strcmp(glyph,"Y"))return "xbox_button_color_y";
    if(!std::strcmp(glyph,"LB"))return "xbox_lb";
    if(!std::strcmp(glyph,"RB"))return "xbox_rb";
    if(!std::strcmp(glyph,"LT"))return "xbox_lt";
    if(!std::strcmp(glyph,"RT"))return "xbox_rt";
    if(!std::strcmp(glyph,"Back/Select"))return "xbox_button_back";
    if(!std::strcmp(glyph,"Start"))return "xbox_button_start";
    if(!std::strcmp(glyph,"Enter"))return "keyboard_enter";
    if(!std::strcmp(glyph,"Esc"))return "keyboard_escape";
    if(!std::strcmp(glyph,"arrows"))return "keyboard_arrows_all";
    if(!std::strcmp(glyph,"keys-horizontal"))return "keyboard_arrows_horizontal";
    if(!std::strcmp(glyph,"dpad"))return "xbox_dpad";
    if(!std::strcmp(glyph,"horizontal"))return "xbox_dpad_horizontal";
    return "generic_button_circle_fill";
}
// A screen's own button, shown after the standard ones.
struct LegendHint { const char* glyph; std::string label; };
// GameMenu.hxx; declared here so the legend reports its labels too.
void ReportMenuText(const char* id,float textHeight,float interiorHeight,float textWidth,float availableWidth);
inline float MenuLegend(float width,const char* select,const char* back,bool draw,bool adjustable,SelectionArt* art,float scale=0,
                        const char* primary=loc::T("menu.select"),const std::vector<LegendHint>& extras={},const char* backText=nullptr) {
    const bool keyboard=!std::strcmp(select,"Enter");
    // A button without its own prompt art (LP, a stick's "1") is drawn as a
    // plain circle, so its label names it.
    const auto prefix=[](const char* glyph){
        return !std::strcmp(PromptAsset(glyph),"generic_button_circle_fill")?std::string(glyph)+" ":std::string();
    };
    const std::string action=prefix(select)+(primary?primary:"");
    const char* backWord=backText&&*backText?backText:loc::T("common.back");
    const std::string backLabel=prefix(back)+backWord;
    std::vector<LegendHint> hints{{keyboard?"arrows":"dpad",loc::T("menu.navigate")}};
    if(primary)hints.push_back({select,action});
    hints.push_back({back,backLabel});
    if(adjustable)hints.push_back({keyboard?"keys-horizontal":"horizontal",loc::T("menu.adjust")});
    hints.insert(hints.end(),extras.begin(),extras.end());
    const float s=scale>0?scale:Scale(),height=38*s,size=32*s;
    float measuredLabels=0;
    for(const auto& hint:hints)measuredLabels+=ImGui::GetFont()->CalcTextSizeA(16*s,FLT_MAX,0,hint.label.c_str()).x;
    const float fixed=hints.size()*(size+32*s);
    const float font=(std::max)(11*s,(std::min)(16*s,16*s*(std::max)(1.f,width-fixed)/(std::max)(1.f,measuredLabels)));
    float x=0,y=0;const auto start=ImGui::GetCursorScreenPos();
    for(const auto& hint:hints){
        const float w=size+8*s+ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,hint.label.c_str()).x+24*s;
        if(x&&x+w>width){x=0;y+=height;}
        if(draw){
            // The font shrinks and rows wrap, so only a hint wider than the
            // whole legend can be cut off.
            ReportMenuText(("legend/"+hint.label).c_str(),font,height,w,width);
            auto* d=ImGui::GetWindowDrawList();
            const auto icon=art?art->InputPrompt(PromptAsset(hint.glyph)):SelectionImage{};
            if(icon.texture)d->AddImage(icon.texture,ImVec2(start.x+x,start.y+y),ImVec2(start.x+x+size,start.y+y+size),icon.uvMin,icon.uvMax);
            else d->AddText(ImGui::GetFont(),12*s,ImVec2(start.x+x,start.y+y+8*s),palette::Ivory,hint.glyph);
            d->AddText(ImGui::GetFont(),font,ImVec2(start.x+x+size+8*s,start.y+y+(size-font)*.5f),palette::Ivory,hint.label.c_str());
        }
        x+=w;
    }
    return y+height;
}
} }
