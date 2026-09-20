#pragma once
#include "Theme.hxx"
#include "SelectionArt.hxx"
#include "../common/Localization.hxx"
#include <cstring>

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
inline float MenuLegend(float width,const char* select,const char* back,bool draw,bool adjustable,SelectionArt* art,float scale=0,const char* primary=loc::T("menu.select")) {
    const bool keyboard=!std::strcmp(select,"Enter");
    const char* glyphs[]={keyboard?"arrows":"dpad",select,back,keyboard?"keys-horizontal":"horizontal"};
    const std::string action=std::string(!std::strcmp(select,"LP")?"LP ":"")+(primary?primary:"");
    const std::string backLabel=!std::strcmp(back,"LK")?std::string("LK ")+loc::T("common.back"):loc::T("common.back");
    const char* labels[]={loc::T("menu.navigate"),action.c_str(),backLabel.c_str(),loc::T("menu.adjust")};
    const float s=scale>0?scale:Scale(),height=38*s,size=32*s;
    const int count=adjustable?4:3;
    float measuredLabels=0;int visible=0;
    for(int i=0;i<count;++i)if(i!=1||primary){measuredLabels+=ImGui::GetFont()->CalcTextSizeA(16*s,FLT_MAX,0,labels[i]).x;++visible;}
    const float fixed=visible*(size+32*s);
    const float font=(std::max)(11*s,(std::min)(16*s,16*s*(std::max)(1.f,width-fixed)/(std::max)(1.f,measuredLabels)));
    float x=0,y=0;const auto start=ImGui::GetCursorScreenPos();
    for(int i=0;i<count;++i){
        if(i==1&&!primary)continue;
        const float w=size+8*s+ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,labels[i]).x+24*s;
        if(x&&x+w>width){x=0;y+=height;}
        if(draw){
            auto* d=ImGui::GetWindowDrawList();
            const auto icon=art?art->InputPrompt(PromptAsset(glyphs[i])):SelectionImage{};
            if(icon.texture)d->AddImage(icon.texture,ImVec2(start.x+x,start.y+y),ImVec2(start.x+x+size,start.y+y+size),icon.uvMin,icon.uvMax);
            else d->AddText(ImGui::GetFont(),12*s,ImVec2(start.x+x,start.y+y+8*s),palette::Ivory,glyphs[i]);
            d->AddText(ImGui::GetFont(),font,ImVec2(start.x+x+size+8*s,start.y+y+(size-font)*.5f),palette::Ivory,labels[i]);
        }
        x+=w;
    }
    return y+height;
}
} }
