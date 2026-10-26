#include "RoomCards.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "MenuProbes.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace sf4e { namespace ui {
namespace {
// Options over a card's footer. They are real buttons, submitted before the
// card so they own the hover and popups still block them; the card is then
// laid out from the same origin.
struct Strip{std::vector<ImVec2> min,max;int clicked=-1,hovered=-1;};
Strip StripButtons(const std::string& id,ImVec2 p,float width,float h,std::size_t count) {
 const float s=Scale();
 Strip strip;strip.min.resize(count);strip.max.resize(count);
 const float stripHeight=26*s,y=p.y+h-stripHeight-3*s,optionWidth=(width-24*s-(count-1)*8*s)/count;
 ImGui::PushID(id.c_str());
 for(int i=0;i<static_cast<int>(count);++i){
  strip.min[i]=ImVec2(p.x+12*s+i*(optionWidth+8*s),y);strip.max[i]=ImVec2(strip.min[i].x+optionWidth,y+stripHeight);
  ImGui::SetCursorScreenPos(strip.min[i]);ImGui::PushID(i);
  if(ImGui::InvisibleButton("##strip",ImVec2(optionWidth,stripHeight)))strip.clicked=i;
  if(ImGui::IsItemHovered())strip.hovered=i;
  ImGui::PopID();ReportMenuCard((id+"/"+std::to_string(i)).c_str(),strip.min[i],strip.max[i]);
 }
 ImGui::PopID();ImGui::SetCursorScreenPos(p);
 return strip;
}
// `chosen` is the lit option; below zero the hovered one is.
void DrawStrip(std::string& elided,const Strip& strip,const std::vector<TableCardOption>& options,int chosen) {
 const float s=Scale();
 auto* d=ImGui::GetWindowDrawList();
 for(int i=0;i<static_cast<int>(options.size());++i){
  const bool lit=chosen>=0?i==chosen:i==strip.hovered;
  d->AddRectFilled(strip.min[i],strip.max[i],lit?IM_COL32(120,64,30,245):IM_COL32(20,19,18,240),4*s);
  if(lit)d->AddRect(strip.min[i],strip.max[i],palette::Ember,4*s,0,2*s);
  const float stripHeight=strip.max[i].y-strip.min[i].y,optionWidth=strip.max[i].x-strip.min[i].x;
  // Our own action labels must fit; the probe holds every locale to that.
  if(MenuProbesActive())ReportMenuText(("board-strip-"+options[i].label).c_str(),14*s,stripHeight,
   ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,options[i].label.c_str()).x,optionWidth-12*s);
  DrawCardText(elided,ImVec2(strip.min[i].x+6*s,strip.min[i].y+(stripHeight-14*s)*.5f),optionWidth-12*s,options[i].label,14*s,
   options[i].live?palette::Ivory:palette::Muted,true);
 }
}
// What the pointer did, the strip's buttons taking precedence over the card's.
CardInteraction Interaction(const Strip& strip,bool pressed,bool hovered) {
 const auto& io=ImGui::GetIO();
 CardInteraction hit;hit.pointerMoved=io.MouseDelta.x!=0||io.MouseDelta.y!=0;
 if(pressed){hit.kind=CardInteraction::Kind::Pressed;}
 else if(strip.clicked>=0){hit.kind=CardInteraction::Kind::Pressed;hit.option=strip.clicked;}
 else if(strip.hovered>=0){hit.kind=CardInteraction::Kind::Hovered;hit.option=strip.hovered;}
 else if(hovered)hit.kind=CardInteraction::Kind::Hovered;
 return hit;
}
}
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
bool DrawCardButton(const std::string& id,ImVec2 size,bool focused,bool scrollTo,bool disabled) {
 const float s=Scale();
 ImGui::PushID(id.c_str());
 ImGui::PushStyleColor(ImGuiCol_Button,focused?ImVec4(.40f,.23f,.12f,.9f):ImVec4(.10f,.09f,.08f,.94f));
 // An open choice leaves only its own options live.
 ImGui::BeginDisabled(disabled);
 const bool clicked=ImGui::Button("##room-entry",size);
 ImGui::EndDisabled();
 ImGui::PopStyleColor();ImGui::PopID();
 if(focused){
  ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),palette::Ember,0,0,2*s);
  if(scrollTo)ImGui::SetScrollHereY(.5f);
 }
 return clicked;
}
CardInteraction DrawTableCard(const TableCardView& card) {
 const float s=Scale(),h=card.height;
 const auto p=ImGui::GetCursorScreenPos();
 const float width=ImGui::GetContentRegionAvail().x;
 // Names ask the atlas for glyphs only while their card is inside the clip.
 const bool cardVisible=ImGui::IsRectVisible(p,ImVec2(p.x+width,p.y+h));
 std::string elided;
 const Strip strip=card.options.empty()?Strip{}:StripButtons(card.id,p,width,h,card.options.size());
 const bool pressed=DrawCardButton(card.id,ImVec2(width,h),card.focused,card.focusChanged,card.disabled);
 const bool hovered=ImGui::IsItemHovered();
 ReportMenuCard(card.id.c_str(),p,ImVec2(p.x+width,p.y+h));
 const auto text=[&](ImVec2 at,float w,const std::string& value,float size,ImU32 color,bool centred=false){
  DrawCardText(elided,at,w,value,size,color,centred);
 };
 // A seat's country or class, named while the pointer is on its mark, and its
 // ping on the pill.
 std::string markTip,pingTip;
 const ImU32 bannerColor=ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
 text(ImVec2(p.x+12*s,p.y+7*s),width*.57f,loc::Tf("room.battle_slot",card.slot+1),16*s,palette::Ivory);
 text(ImVec2(p.x+width*.60f,p.y+8*s),width*.40f-12*s,card.phase,14*s,palette::Ember);
 // A full pair shows its running win count where "VS" would sit; the gap
 // between the two sides grows to fit however long the rematch run gets.
 const float gap=(std::max)(34*s,ImGui::GetFont()->CalcTextSizeA(18*s,FLT_MAX,0,card.middle.c_str()).x+12*s);
 // A banner takes a row above the footer, so the option strip never hides it.
 const float bannerHeight=card.banner.empty()?0:TableBannerHeight*s,bannerTop=p.y+h-32*s-bannerHeight;
 // Portraits sit on the card's outer edges, mirrored, so neither crowds the
 // score. The row fills and centres in the band between header and footer.
 // The footer band holds the rules line or, over it, an option strip, so
 // portraits and names never sit under the strip.
 const float band=h-60*s-bannerHeight,portrait=(std::min)(96*s,band),half=(width-24*s-gap)*.5f;
 const float top=p.y+28*s+(band-portrait)*.5f+portrait*.5f-20*s;
 for(int side=0;side<2;++side){
  const auto& seat=card.seats[side];
  const float x=p.x+12*s+side*(half+gap);
  const float px=side?x+half-portrait:x,py=p.y+28*s+(band-portrait)*.5f;
  if(seat.occupied)DrawCharacterPortrait(seat.fighter,ImVec2(px,py),ImVec2(px+portrait,py+portrait));
  if(seat.occupied&&seat.marked)ImGui::GetWindowDrawList()->AddRect(ImVec2(px-2*s,py-2*s),ImVec2(px+portrait+2*s,py+portrait+2*s),bannerColor,3*s,0,2*s);
  // The link mark rides the portrait's inner lower corner on a dark disc.
  if(seat.occupied){
   const float mark=14*s;const ImVec2 centre(side?px+10*s:px+portrait-10*s,py+portrait-10*s);
   ImGui::GetWindowDrawList()->AddCircleFilled(centre,11*s,IM_COL32(20,19,18,225));
   DrawNetworkLinkGlyph(ImGui::GetWindowDrawList(),ImVec2(centre.x-mark*.5f,centre.y-mark*.5f),mark,seat.link);
   // The ping sits in a pill beside the disc, towards the portrait's middle.
   ImVec2 hoverMin(centre.x-11*s,centre.y-11*s),hoverMax(centre.x+11*s,centre.y+11*s);
   if(!seat.latency.text.empty()){
    const float size=12*s,textWidth=ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,seat.latency.text.c_str()).x;
    const float left=side?centre.x+13*s:centre.x-13*s-textWidth-10*s;
    const ImVec2 min(left,centre.y-8*s),max(left+textWidth+10*s,centre.y+8*s);
    ImGui::GetWindowDrawList()->AddRectFilled(min,max,IM_COL32(20,19,18,225),8*s);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),size,ImVec2(min.x+5*s,centre.y-size*.5f),seat.latency.color,seat.latency.text.c_str());
    hoverMin.x=(std::min)(hoverMin.x,min.x);hoverMax.x=(std::max)(hoverMax.x,max.x);
   }
   if(!seat.latency.tip.empty()&&ImGui::IsMouseHoveringRect(hoverMin,hoverMax))pingTip=seat.latency.tip;
  }
  // A readied fighter wears a badge on the portrait until the game starts.
  if(seat.occupied&&seat.ready){
   const char* badge=loc::T("room.status.ready");const float size=13*s;
   const float badgeWidth=ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,badge).x+12*s;
   const ImVec2 min(px+(portrait-badgeWidth)*.5f,py+4*s),max(min.x+badgeWidth,min.y+size+4*s);
   ImGui::GetWindowDrawList()->AddRectFilled(min,max,palette::Ready,(size+4*s)*.5f);
   ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),size,ImVec2(min.x+6*s,min.y+2*s),IM_COL32(20,19,18,255),badge);
  }
  const float tx=x+(seat.occupied&&!side?portrait+8*s:0),tw=half-(seat.occupied?portrait+8*s:0);
  if(seat.occupied&&cardVisible)NoteUserText(seat.name);
  // Names of different lengths read ragged when flush left; centre each
  // in its own slot so the pair stays symmetric about VS. The flag and the
  // class emblem lead the name, as in the Members list, and centre with it.
  if(seat.occupied){
   const float size=18*s,slot=NameMarksWidth(seat.marks,size);
   const float measured=(std::min)(tw-slot,ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,seat.name.c_str()).x);
   const float left=tx+(std::max)(0.f,(tw-slot-measured)*.5f);
   const auto marks=DrawNameMarks(ImGui::GetWindowDrawList(),left,top,size,seat.marks);
   text(ImVec2(left+marks.width,top),tw-(left-tx)-marks.width,seat.name,size,palette::Ivory);
   if(!marks.tip.empty())markTip=marks.tip;
  }else text(ImVec2(tx,top),tw,loc::T("room.looking_for_fight"),18*s,palette::Muted,true);
  // The fighter who has won sets back to back at this table. A banner takes
  // this line's place when the card is too short for both.
  const bool thirdLine=!bannerHeight||top+53*s<=bannerTop;
  text(ImVec2(tx,top+23*s),tw,seat.caption,14*s,palette::Muted,true);
  if(seat.occupied&&thirdLine){
   const bool streak=!seat.streak.empty();
   text(ImVec2(tx,top+40*s),tw,streak?seat.streak:seat.idle,13*s,streak?palette::Ember:palette::Muted,true);
  }
 }
 text(ImVec2(p.x+12*s+half,top+16*s),gap,card.middle,18*s,palette::Ember,true);
 if(bannerHeight){
  const ImVec2 min(p.x+8*s,bannerTop),max(p.x+width-8*s,bannerTop+bannerHeight-2*s);
  ImGui::GetWindowDrawList()->AddRectFilled(min,max,IM_COL32(74,46,20,235),4*s);
  ImGui::GetWindowDrawList()->AddRect(min,max,bannerColor,4*s,0,1*s);
  if(cardVisible)NoteUserText(card.banner);
  text(ImVec2(min.x+8*s,min.y+(max.y-min.y-13*s)*.5f),max.x-min.x-16*s,card.banner,13*s,bannerColor);
  ReportMenuCard((card.id+"/banner").c_str(),min,max);
 }
 // An option strip covers the footer, so the footer gives way to it.
 if(card.options.empty())text(ImVec2(p.x+12*s,p.y+h-22*s),width-24*s,card.footer,14*s,palette::Muted);
 CardInteraction hit=Interaction(strip,pressed,hovered);
 if(card.choosing){
  // The chosen side follows a moving mouse; a resting cursor must not undo
  // what the pad or keyboard just picked. The board makes the same choice
  // from the interaction, so the highlight and the choice agree this frame.
  const int chosen=hit.option>=0&&hit.pointerMoved?hit.option:card.chosen;
  // The seat a sit option would take blinks.
  const int seat=chosen>=0&&chosen<static_cast<int>(card.options.size())?card.options[chosen].seat:-1;
  if(seat>=0){
   const float pulse=.55f+.45f*std::sin(static_cast<float>(ImGui::GetTime())*6.f);
   const float x=p.x+12*s+seat*(half+gap);
   ImGui::GetWindowDrawList()->AddRect(ImVec2(x-4*s,p.y+26*s),ImVec2(x+half+4*s,p.y+h-32*s),ImGui::GetColorU32(ImVec4(1.f,.53f,.22f,pulse)),4*s,0,3*s);
  }
  DrawStrip(elided,strip,card.options,chosen);
 }else if(!card.options.empty())DrawStrip(elided,strip,card.options,-1);
 if(!markTip.empty())elided=markTip;
 else if(!pingTip.empty())elided=pingTip;
 FlushCardTip(elided);
 return hit;
}
CardInteraction DrawMemberCard(const MemberCardView& card) {
 const float s=Scale();
 const auto p=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
 std::string elided;
 if(ImGui::IsRectVisible(p,ImVec2(p.x+width,p.y+58*s)))NoteUserText(card.name);
 const bool pressed=DrawCardButton(card.id,ImVec2(width,58*s),card.focused,card.focusChanged,card.disabled);
 const bool hovered=ImGui::IsItemHovered();
 DrawCharacterPortrait(card.mainFighter,ImVec2(p.x+6*s,p.y+5*s),ImVec2(p.x+54*s,p.y+53*s));
 // The flag and the class lead the name; with neither the name keeps its place.
 const auto marks=DrawNameMarks(ImGui::GetWindowDrawList(),p.x+64*s,p.y+7*s,16*s,card.marks);
 // The ping sits left of the link mark, and the name gives way to it.
 float ping=0;
 if(!card.latency.text.empty()){
  ping=ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,card.latency.text.c_str()).x;
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),14*s,ImVec2(p.x+width-34*s-ping,p.y+9*s),card.latency.color,card.latency.text.c_str());
  ping+=6*s;
 }
 DrawCardText(elided,ImVec2(p.x+64*s+marks.width,p.y+7*s),width-98*s-marks.width-ping,card.label,16*s,palette::Ivory);
 DrawNetworkLinkGlyph(ImGui::GetWindowDrawList(),ImVec2(p.x+width-28*s,p.y+8*s),16*s,card.link);
 // A controller player reads the mark (or the Members list); a mouse can
 // also have it in words, with the ping.
 elided+=(elided.empty()?"":"\n")+std::string(NetworkLinkName(card.link));
 if(!card.latency.tip.empty())elided+="\n"+card.latency.tip;
 DrawCardText(elided,ImVec2(p.x+64*s,p.y+31*s),width-76*s,card.status,14*s,palette::Muted);
 ReportMenuCardText(card.id,card.status);
 // Over the flag or the emblem, the tip names the country or the class.
 if(!marks.tip.empty())elided=marks.tip;
 FlushCardTip(elided);
 return Interaction({},pressed,hovered);
}
} }
