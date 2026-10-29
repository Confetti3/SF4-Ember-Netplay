#include "RoomCards.hxx"
#include "V2.hxx"
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
struct Strip{std::vector<ImVec2> min,max;int clicked=-1,hovered=-1;float extraHeight=0;};
Strip StripButtons(const std::string& id,ImVec2 p,float width,float h,const std::vector<TableCardOption>& options) {
 const auto count=options.size();
 const float s=Scale();
 Strip strip;strip.min.resize(count);strip.max.resize(count);
 const float stripHeight=26*s,available=width-24*s;
 std::vector<float> widths;
 for(const auto& option:options)widths.push_back(ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,option.label.c_str()).x+12*s);
 std::size_t columns=count;
 for(;columns>1;--columns){
  bool fits=true;
  for(std::size_t first=0;first<count;first+=columns){
   const auto end=(std::min)(count,first+columns);float needed=(end-first-1)*8*s;
   for(auto i=first;i<end;++i)needed+=widths[i];
   if(needed>available){fits=false;break;}
  }
  if(fits)break;
 }
 strip.extraHeight=((count+columns-1)/columns-1)*(stripHeight+4*s);
 float x=0,spare=0;
 ImGui::PushID(id.c_str());
 for(int i=0;i<static_cast<int>(count);++i){
  if(i%columns==0){
   x=p.x+12*s;const auto end=(std::min)(count,static_cast<std::size_t>(i)+columns);
   float needed=(end-i-1)*8*s;for(auto n=static_cast<std::size_t>(i);n<end;++n)needed+=widths[n];
   spare=(available-needed)/(end-i);
  }
  const float optionWidth=widths[i]+spare,y=p.y+h-stripHeight-3*s+(i/columns)*(stripHeight+4*s);
  strip.min[i]=ImVec2(x,y);strip.max[i]=ImVec2(x+optionWidth,y+stripHeight);x+=optionWidth+8*s;
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
 const bool look=v2::On();
 ImGui::PushStyleColor(ImGuiCol_Button,look?ImGui::ColorConvertU32ToFloat4(focused?v2::Colors().header:v2::color::Alpha(v2::Colors().frame,.94f)):
  focused?ImVec4(.40f,.23f,.12f,.9f):ImVec4(.10f,.09f,.08f,.94f));
 ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,look?4*s:ImGui::GetStyle().FrameRounding);
 // An open choice leaves only its own options live.
 ImGui::BeginDisabled(disabled);
 const bool clicked=ImGui::Button("##room-entry",size);
 ImGui::EndDisabled();
 ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::PopID();
 if(focused){
  if(look)v2::Focus(ImGui::GetWindowDrawList(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
  else ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),palette::Ember,0,0,2*s);
  if(scrollTo)ImGui::SetScrollHereY(.5f);
 }
 return clicked;
}
CardInteraction DrawTableCard(const TableCardView& card) {
 const float s=Scale(),contentHeight=card.height;
 const auto p=ImGui::GetCursorScreenPos();
 const float width=ImGui::GetContentRegionAvail().x;
 // Names ask the atlas for glyphs only while their card is inside the clip.
 const bool cardVisible=ImGui::IsRectVisible(p,ImVec2(p.x+width,p.y+contentHeight));
 std::string elided;
 const Strip strip=card.options.empty()?Strip{}:StripButtons(card.id,p,width,contentHeight,card.options);
 const float h=contentHeight+strip.extraHeight;
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
 if(!card.name.empty()&&cardVisible)NoteUserText(card.name);
 text(ImVec2(p.x+12*s,p.y+7*s),width*.57f,loc::Tf("room.battle_slot",card.slot+1)+(card.name.empty()?std::string():" \xC2\xB7 "+card.name),16*s,palette::Ivory);
 // V2: the table's state as a chip at the card's right end.
 if(v2::On()&&!card.phase.empty())v2::Chip(ImGui::GetWindowDrawList(),ImVec2(p.x+width-12*s-v2::ChipWidth(card.phase),p.y+6*s),card.phase,palette::Ember);
 else text(ImVec2(p.x+width*.60f,p.y+8*s),width*.40f-12*s,card.phase,14*s,palette::Ember);
 // A full pair shows its running win count where "VS" would sit; the gap
 // between the two sides grows to fit however long the rematch run gets.
 const float gap=(std::max)(34*s,ImGui::GetFont()->CalcTextSizeA(18*s,FLT_MAX,0,card.middle.c_str()).x+12*s);
 // A banner takes a row above the footer, so the option strip never hides it.
 const float bannerHeight=card.banner.empty()?0:TableBannerHeight*s,bannerTop=p.y+contentHeight-32*s-bannerHeight;
 // Portraits sit on the card's outer edges, mirrored, so neither crowds the
 // score. The row fills and centres in the band between header and footer.
 // The footer band holds the rules line or, over it, an option strip, so
 // portraits and names never sit under the strip.
 const float band=contentHeight-60*s-bannerHeight,portrait=(std::min)(96*s,band),half=(width-24*s-gap)*.5f;
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
 if(ImGui::IsRectVisible(p,ImVec2(p.x+width,p.y+MemberCardHeight*s)))NoteUserText(card.name);
 const bool pressed=DrawCardButton(card.id,ImVec2(width,MemberCardHeight*s),card.focused,card.focusChanged,card.disabled);
 const bool hovered=ImGui::IsItemHovered();
 DrawCharacterPortrait(card.mainFighter,ImVec2(p.x+6*s,p.y+5*s),ImVec2(p.x+54*s,p.y+53*s));
 if(v2::On()){
  // V2: the player tag (flag, class, name, ping) over the status line.
  v2::Tag(ImGui::GetWindowDrawList(),ImVec2(p.x+64*s,p.y+6*s),width-98*s,{card.label,{},{},card.marks,card.latency},v2::TagSize::S);
  DrawNetworkLinkGlyph(ImGui::GetWindowDrawList(),ImVec2(p.x+width-28*s,p.y+8*s),16*s,card.link);
  elided=NetworkLinkName(card.link);
  if(!card.latency.tip.empty())elided+="\n"+card.latency.tip;
  DrawCardText(elided,ImVec2(p.x+64*s,p.y+33*s),width-76*s,card.status,14*s,palette::Muted);
  ReportMenuCardText(card.id,card.status);
  FlushCardTip(elided);
  return Interaction({},pressed,hovered);
 }
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
void DrawGrip(const char* id,ImVec2 at,ImVec2 size) {
    const float s=Scale();
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(id,size);
    const bool vertical=size.y>size.x,hot=ImGui::IsItemActive()||ImGui::IsItemHovered();
    if(hot)ImGui::SetMouseCursor(vertical?ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_ResizeNS);
    auto* draw=ImGui::GetWindowDrawList();
    const ImVec2 c(at.x+size.x*.5f,at.y+size.y*.5f),step(vertical?0:6*s,vertical?6*s:0);
    // The line shows at rest, as a panel edge does, and lights while held.
    draw->AddLine(vertical?ImVec2(c.x,at.y):ImVec2(at.x,c.y),vertical?ImVec2(c.x,at.y+size.y):ImVec2(at.x+size.x,c.y),hot?palette::Ember:ImGui::GetColorU32(ImGuiCol_Border),hot?2*s:s);
    for(int dot=-2;dot<=2;++dot)draw->AddCircleFilled(ImVec2(c.x+dot*step.x,c.y+dot*step.y),2.5f*s,hot?palette::Ivory:palette::Muted);
}
bool DrawPartHeading(const char* id,const std::string& title,bool shown,bool focused,const std::string& tip,float& titleRight) {
    const float s=Scale(),h=ImGui::GetFontSize()+8*s,width=ImGui::GetContentRegionAvail().x,box=h-8*s,size=14*s;
    const auto p=ImGui::GetCursorScreenPos();
    // The whole line is the button: the arrow in its box, then the part's name.
    const bool pressed=ImGui::InvisibleButton(id,ImVec2(width,h)),hot=focused||ImGui::IsItemHovered();
    auto* draw=ImGui::GetWindowDrawList();
    const ImVec2 at(p.x,p.y+4*s);
    if(hot)draw->AddRectFilled(at,ImVec2(at.x+box,at.y+box),palette::Ember,4*s);
    else draw->AddRect(at,ImVec2(at.x+box,at.y+box),palette::Muted,4*s,0,s);
    // Pointing down while the part shows, right while it is hidden.
    const float x=at.x+box*.5f,y=at.y+box*.5f,d=3.5f*s;
    const ImU32 arrow=hot?ImGui::GetColorU32(ImGuiCol_FrameBg):palette::Ivory;
    if(shown)draw->AddTriangleFilled(ImVec2(x-1.5f*d,y-d),ImVec2(x+1.5f*d,y-d),ImVec2(x,y+d),arrow);
    else draw->AddTriangleFilled(ImVec2(x-d,y-1.5f*d),ImVec2(x-d,y+1.5f*d),ImVec2(x+d,y),arrow);
    const float left=at.x+box+10*s;
    // A narrow column cuts the name with "...", and the hover shows it whole.
    std::string elided;
    DrawCardText(elided,ImVec2(left,p.y+(h-size)*.5f),p.x+width-left,title,size,hot?palette::Ivory:palette::Muted);
    titleRight=(std::min)(p.x+width,left+ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,title.c_str()).x);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",elided.empty()?tip.c_str():(title+"\n"+tip).c_str());
    return pressed;
}
void DrawMemberRow(const MemberCardView& card,ImVec2 min,ImVec2 max) {
 const float s=Scale(),h=max.y-min.y,face=h-12*s;auto* d=ImGui::GetWindowDrawList();
 DrawCharacterPortrait(card.mainFighter,ImVec2(min.x+6*s,min.y+6*s),ImVec2(min.x+6*s+face,max.y-6*s));
 const float x=min.x+16*s+face,w=max.x-x-44*s;
 v2::Tag(d,ImVec2(x,min.y+5*s),w,{card.label,{},{},card.marks,card.latency},v2::TagSize::S);
 std::string elided;
 DrawCardText(elided,ImVec2(x,max.y-20*s),w,card.status,14*s,palette::Muted);
 DrawNetworkLinkGlyph(d,ImVec2(max.x-30*s,min.y+(h-16*s)*.5f),16*s,card.link);
 NoteUserText(card.name);
}
} }
