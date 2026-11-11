#include "RoomCards.hxx"
#include "V2.hxx"
#include "GameMenu.hxx"
#include "Theme.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
void DrawCardText(std::string& elided,ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred) {
 if(width<=0)return;
 auto* font=ImGui::GetFont();std::string label=value;
 float measured=font->CalcTextSizeA(size,FLT_MAX,0,label.c_str()).x;
 if(measured>width){
  const char* end=nullptr;const float dots=font->CalcTextSizeA(size,FLT_MAX,0,"...").x;
  font->CalcTextSizeA(size,(std::max)(1.f,width-dots),0,label.c_str(),nullptr,&end);
  label=std::string(label.c_str(),end)+"...";
  elided+=(elided.empty()?"":"\n")+value;
  measured=font->CalcTextSizeA(size,FLT_MAX,0,label.c_str()).x;
 }
 if(centred)p.x+=(std::max)(0.f,(width-measured)*.5f);
 ImGui::GetWindowDrawList()->AddText(font,size,p,color,label.c_str());
}
void FlushCardTip(std::string& elided) {
 if(!elided.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",elided.c_str());
 elided.clear();
}
void DrawMemberRow(const MemberCardView& card,ImVec2 min,ImVec2 max) {
 const float s=Scale(),h=max.y-min.y,face=h-12*s;auto* d=ImGui::GetWindowDrawList();
 const v2::Box portrait{ImVec2(min.x+6*s,min.y+6*s),ImVec2(min.x+6*s+face,max.y-6*s)};
 DrawCharacterPortrait(FighterPicture{card.mainFighter,card.art},portrait.min,portrait.max);
 if(card.points)v2::LevelBand(d,portrait,card.points->points);
 const float x=min.x+16*s+face,w=max.x-x-14*s;
 v2::TagView tag{card.label,{},{},card.marks,card.latency};tag.link=card.link;
 v2::Tag(d,ImVec2(x,min.y+5*s),w,tag,v2::TagSize::S);
 std::string elided;
 DrawCardText(elided,ImVec2(x,max.y-20*s),w,card.record.empty()?card.status:card.status+" \xC2\xB7 "+card.record,14*s,palette::Muted);
 NoteUserText(card.name);
}
void DrawMemberPoints(const MemberPoints& points,const std::string& name,const FighterPicture& face) {
 if(!points.known)return;
 const float s=Scale(),side=72*s,gap=12*s,line=16*s,width=ImGui::GetContentRegionAvail().x;
 const auto& c=v2::Colors();auto* d=ImGui::GetWindowDrawList();
 const ImVec2 start=ImGui::GetCursorScreenPos(),top(start.x,start.y+12*s);
 DrawCharacterPortrait(face,top,ImVec2(top.x+side,top.y+side),c.frame);
 // SF4's lines under the name: the points of the fighter they are playing, then the account's.
 const float x=top.x+side+gap,room=(std::max)(0.f,width-side-gap);
 NoteUserText(name);
 v2::Text(d,ImVec2(x,top.y),room,name,18*s,c.text,HeadingFont());
 const auto played=points.Of(face.fighter);
 v2::Level(d,ImVec2(x,top.y+28*s),line,played?played->points:0,"points.label");
 v2::Level(d,ImVec2(x,top.y+52*s),line,points.account.points,"points.account_label");
 ImGui::Dummy(ImVec2(width,12*s+side));
}
} }
