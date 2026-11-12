#include "RoomCards.hxx"
#include "V2.hxx"
#include "GameMenu.hxx"
#include "PlayerCard.hxx"
#include "RoomPanelBoard.hxx"
#include "ShellView.hxx"
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
OtherCardView MemberOtherCard(const ShellView& view,const room::Member& member,FighterArt art) {
 const auto points=PointsOf(view,member);
 const auto look=SeatLookOf(view,member);
 const auto& c=v2::Colors();
 OtherCardView card;
 card.name=member.name;card.country=member.country;card.badge=member.badge;
 card.fighter=look.fighter;card.art=art;
 // As the member's seat, cabinet and row: no card read, no points drawn.
 card.points=points.known;
 if(const auto played=points.Of(look.fighter)){card.fighterPoints=played->points;card.fighterTier=played->tier;}
 card.accountPoints=points.account.points;
 // A friend and a request sent are states to show; a request received is the page's Accept row.
 using netplay::levels::Relation;
 if(points.relation==Relation::Friend){card.chip=loc::T("friends.are_friends");card.chipTone=c.ready;}
 else if(points.relation==Relation::Sent){card.chip=loc::T("friends.request_sent");card.chipTone=c.muted;}
 return card;
}
} }
