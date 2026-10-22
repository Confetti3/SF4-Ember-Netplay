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
  ReportMenuText(("board-strip-"+options[i].label).c_str(),14*s,stripHeight,
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
 // V2: the table's state as a chip at the card's right end.
 if(v2::On()&&!card.phase.empty())v2::Chip(ImGui::GetWindowDrawList(),ImVec2(p.x+width-12*s-v2::ChipWidth(card.phase),p.y+6*s),card.phase,palette::Ember);
 else text(ImVec2(p.x+width*.60f,p.y+8*s),width*.40f-12*s,card.phase,14*s,palette::Ember);
 // A full pair shows its running win count where "VS" would sit; the gap
 // between the two sides grows to fit however long the rematch run gets.
 const float middleSize=(card.stage?56:18)*s;
 const float gap=(std::max)(34*s,ImGui::GetFont()->CalcTextSizeA(middleSize,FLT_MAX,0,card.middle.c_str()).x+(card.stage?48:12)*s);
 // A banner takes a row above the footer, so the option strip never hides it.
 const float bannerHeight=card.banner.empty()?0:TableBannerHeight*s,bannerTop=p.y+h-32*s-bannerHeight;
 // Portraits sit on the card's outer edges, mirrored, so neither crowds the
 // score. The row fills and centres in the band between header and footer.
 // The footer band holds the rules line or, over it, an option strip, so
 // portraits and names never sit under the strip.
 const float band=h-60*s-bannerHeight,portrait=(std::min)((card.stage?220:96)*s,band),half=(width-24*s-gap)*.5f;
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
 text(ImVec2(p.x+12*s+half,card.stage?top+20*s-middleSize*.5f:top+16*s),gap,card.middle,middleSize,card.stage?v2::Colors().accent:palette::Ember,true);
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
namespace {
// A cabinet's width for its height, as DrawCabinet draws it.
constexpr float CabinetAspect=.95f;
constexpr ImU32 CabinetNeon=IM_COL32(222,36,44,255);
// The size an empty cabinet's screen writes Insert coin at, for its height.
// Below the smallest readable size the screen stays dark and the text goes
// beside the cabinet instead.
float ScreenTextSize(float height) { return height*.11f; }
bool ScreenTextReadable(float height) { return ScreenTextSize(height)>=11*Scale(); }
// A crown over `centre`, `size` wide.
void DrawCrown(ImDrawList* draw,ImVec2 centre,float size) {
 const float h=size*.6f,x=centre.x-size*.5f,y=centre.y-h*.5f;
 const ImVec2 points[]={{x,y+h},{x,y+h*.2f},{x+size*.25f,y+h*.55f},{x+size*.5f,y},{x+size*.75f,y+h*.55f},{x+size,y+h*.2f},{x+size,y+h}};
 draw->AddConvexPolyFilled(points,7,IM_COL32(240,190,60,255));
}
// A one player cabinet seen from the front, as USF4's arcade cabinets stand:
// a black body with a red neon edge, its number on the marquee (lit in the
// pair's colour while someone plays), the player's fighter on its monitor or
// Insert coin on a dark one, and the control panel with its stick and buttons.
void DrawCabinet(ImDrawList* draw,ImVec2 p,float height,const CabinetTileView& tile) {
 const float s=Scale(),w=height*CabinetAspect;
 const ImU32 body=IM_COL32(16,14,17,255),panel=IM_COL32(70,14,18,255),glass=IM_COL32(20,24,32,255);
 const auto at=[&](float x,float y){return ImVec2(p.x+w*x,p.y+height*y);};
 const auto box=[&](float x0,float y0,float x1,float y1,ImU32 color,float round){draw->AddRectFilled(at(x0,y0),at(x1,y1),color,round*s);};
 // Body and its neon edge, a soft glow under a sharp line.
 box(0,0,1,1,body,4);
 draw->AddRect(at(0,0),at(1,1),(CabinetNeon&0x00FFFFFF)|0x50000000,4*s,0,4*s);
 draw->AddRect(at(0,0),at(1,1),CabinetNeon,4*s,0,1.5f*s);
 // Marquee: the cabinet's number, a crown for a streak, a red line under it.
 box(.05f,.03f,.95f,.17f,tile.occupied&&tile.accent?((tile.accent&0x00FFFFFF)|0x60000000):IM_COL32(32,30,34,255),2);
 box(.05f,.165f,.95f,.18f,CabinetNeon,0);
 std::string unused;
 const float label=(std::max)(8*s,height*.12f);
 DrawCardText(unused,ImVec2(p.x,p.y+height*.10f-label*.5f),w,std::to_string(tile.number),label,palette::Ivory,true);
 if(tile.crown)DrawCrown(draw,ImVec2(p.x+w*.18f,p.y+height*.10f),w*.18f);
 // The monitor.
 box(.07f,.21f,.93f,.66f,glass,2);
 const float top=p.y+height*.23f,bottom=p.y+height*.64f,screen=bottom-top;
 if(tile.occupied){const float x=p.x+(w-screen)*.5f;DrawCharacterPortrait(tile.fighter,ImVec2(x,top),ImVec2(x+screen,bottom));}
 else{
  const float size=ScreenTextSize(height);
  if(ScreenTextReadable(height))DrawCardText(unused,ImVec2(p.x+w*.08f,(top+bottom)*.5f-size*.5f),w*.84f,tile.status,size,palette::Muted,true);
 }
 // Control panel with a stick and two rows of buttons.
 box(.03f,.70f,.97f,.84f,panel,2);
 const float row=p.y+height*.77f,dot=(std::max)(1.f,height*.024f);
 draw->AddCircleFilled(ImVec2(p.x+w*.20f,row-dot*.6f),dot*1.7f,CabinetNeon);
 for(int line=0;line<2;++line)for(int column=0;column<3;++column)
  draw->AddCircleFilled(ImVec2(p.x+w*(.50f+column*.13f),row+(line-.5f)*dot*2.5f),dot,line?IM_COL32(230,230,235,255):IM_COL32(60,130,230,255));
 // Kick plate.
 box(.12f,.88f,.88f,.95f,IM_COL32(40,36,40,255),1);
}
}
bool DrawCabinetTile(const CabinetTileView& tile,ImVec2 size) {
 const float s=Scale();
 const auto p=ImGui::GetCursorScreenPos();
 std::string elided;
 const bool pressed=DrawCardButton(tile.id,size,tile.focused,tile.focusChanged,tile.disabled);
 auto* draw=ImGui::GetWindowDrawList();
 if(tile.accent)draw->AddRect(ImVec2(p.x+3*s,p.y+3*s),ImVec2(p.x+size.x-3*s,p.y+size.y-3*s),tile.accent,4*s,0,2.5f*s);
 // The cabinet on the left; its player's name and flag, fighter and table
 // beside it. An empty cabinet says Insert coin on its screen, or beside it
 // where the screen is too small to read.
 const float cabinet=(std::max)(24*s,size.y-12*s);
 DrawCabinet(draw,ImVec2(p.x+10*s,p.y+6*s),cabinet,tile);
 const float x=p.x+18*s+cabinet*CabinetAspect,width=p.x+size.x-8*s-x,middle=p.y+size.y*.5f;
 if(tile.occupied){
  if(ImGui::IsRectVisible(p,ImVec2(p.x+size.x,p.y+size.y)))NoteUserText(tile.name);
  const auto flag=DrawCountryFlagBeside(draw,x,middle-28*s,15*s,tile.country);
  DrawCardText(elided,ImVec2(x+flag.width,middle-28*s),width-flag.width,tile.name,15*s,palette::Ivory);
  DrawCardText(elided,ImVec2(x,middle-8*s),width,tile.caption,13*s,palette::Muted);
  DrawCardText(elided,ImVec2(x,middle+10*s),width,tile.status,12*s,tile.accent?tile.accent:palette::Muted);
 }else if(!ScreenTextReadable(cabinet))DrawCardText(elided,ImVec2(x,middle-7*s),width,tile.status,13*s,palette::Muted);
 FlushCardTip(elided);
 return pressed;
}
bool DrawRealCabinet(const CabinetTileView& tile,ImVec2 size,const std::string& empty) {
 const float s=Scale(),w=size.x,h=size.y;
 const auto p=ImGui::GetCursorScreenPos();
 ImGui::PushID(tile.id.c_str());
 ImGui::BeginDisabled(tile.disabled);
 const bool pressed=ImGui::InvisibleButton("##cabinet",size);
 ImGui::EndDisabled();
 ImGui::PopID();
 if(tile.focused&&tile.focusChanged)ImGui::SetScrollHereY(.5f);
 auto* draw=ImGui::GetWindowDrawList();
 const auto at=[&](float x,float y){return ImVec2(p.x+w*x,p.y+h*y);};
 const auto box=[&](float x0,float y0,float x1,float y1,ImU32 color,float round){draw->AddRectFilled(at(x0,y0),at(x1,y1),color,round*s);};
 std::string elided;
 // Number tag, in the pair's colour while someone plays.
 box(.36f,0,.64f,.07f,IM_COL32(20,18,22,235),2);
 draw->AddRect(at(.36f,0),at(.64f,.07f),tile.occupied&&tile.accent?tile.accent:IM_COL32(90,86,94,255),2*s,0,1*s);
 DrawCardText(elided,ImVec2(p.x,p.y+h*.035f-8*s),w,std::to_string(tile.number),16*s,palette::Ivory,true);
 // The cabinet: a black body in neon, red while it stands empty and in its
 // pair's colour while anyone plays there, glowing brighter when focused.
 const ImU32 neon=tile.accent?tile.accent:CabinetNeon,rgb=neon&0x00FFFFFF;
 const ImVec2 top=at(.04f,.09f),bottom=at(.96f,.78f);
 const int glow=tile.focused?5:tile.accent?3:0;
 for(int ring=glow;ring>=1;--ring)draw->AddRect(ImVec2(top.x-ring*3*s,top.y-ring*3*s),ImVec2(bottom.x+ring*3*s,bottom.y+ring*3*s),
  rgb|(static_cast<ImU32>((tile.focused?55:30)*(glow+1-ring)/glow)<<24),8*s,0,3*s);
 draw->AddRectFilled(top,bottom,IM_COL32(14,12,15,255),7*s);
 draw->AddRect(top,bottom,rgb|0x60000000,7*s,0,4*s);
 draw->AddRect(top,bottom,neon,7*s,0,tile.focused?2.5f*s:1.5f*s);
 // Marquee, lit in the same colour, with the crown for a streak.
 box(.10f,.11f,.90f,.16f,tile.accent?(rgb|0x90000000):IM_COL32(110,14,22,255),2);
 box(.10f,.155f,.90f,.162f,neon,0);
 if(tile.crown)DrawCrown(draw,ImVec2(p.x+w*.5f,p.y+h*.135f),w*.16f);
 // The screen: the fighter, or Insert coin on dark glass.
 box(.10f,.18f,.90f,.44f,IM_COL32(22,26,34,255),2);
 const float screenTop=p.y+h*.185f,screenBottom=p.y+h*.435f,screen=(std::min)(screenBottom-screenTop,w*.78f);
 if(tile.occupied){const float x=p.x+(w-screen)*.5f;DrawCharacterPortrait(tile.fighter,ImVec2(x,screenTop),ImVec2(x+screen,screenTop+screen));}
 else DrawCardText(elided,ImVec2(p.x+w*.1f,(screenTop+screenBottom)*.5f-7*s),w*.8f,tile.status,14*s,palette::Ivory,true);
 // Under the screen: the player's flag, name and fighter, or the empty line.
 box(.10f,.45f,.90f,.56f,IM_COL32(26,24,28,255),2);
 const float textX=p.x+w*.14f,textWidth=w*.72f,line=p.y+h*.455f;
 if(tile.occupied){
  if(ImGui::IsRectVisible(p,ImVec2(p.x+w,p.y+h)))NoteUserText(tile.name);
  const auto flag=DrawCountryFlagBeside(draw,textX,line,13*s,tile.country);
  // A short cabinet has room under its screen for the name alone.
  const bool caption=h*.11f>=29*s;
  DrawCardText(elided,ImVec2(textX+flag.width,caption?line:line+(h*.11f-13*s)*.5f),textWidth-flag.width,tile.name,13*s,palette::Ivory);
  if(caption)DrawCardText(elided,ImVec2(textX,line+16*s),textWidth,tile.caption,12*s,tile.accent?tile.accent:palette::Muted);
 }else DrawCardText(elided,ImVec2(textX,line+8*s),textWidth,empty,12*s,palette::Muted,true);
 // Control panel: a stick and two rows of buttons, then the lower body.
 draw->AddQuadFilled(at(.06f,.58f),at(.94f,.58f),at(.99f,.66f),at(.01f,.66f),IM_COL32(60,12,16,255));
 const float row=p.y+h*.62f,dot=(std::max)(1.5f,w*.028f);
 draw->AddCircleFilled(ImVec2(p.x+w*.22f,row-dot*.6f),dot*1.8f,neon);
 for(int r=0;r<2;++r)for(int c=0;c<3;++c)
  draw->AddCircleFilled(ImVec2(p.x+w*(.50f+c*.13f),row+(r-.5f)*dot*2.4f),dot,r?IM_COL32(230,230,235,255):neon);
 box(.10f,.67f,.90f,.77f,IM_COL32(20,18,22,255),2);
 // The stool in front: a padded seat on two chrome legs.
 box(.18f,.82f,.82f,.87f,IM_COL32(28,26,30,255),4);
 box(.20f,.82f,.80f,.835f,IM_COL32(70,66,74,255),3);
 const ImU32 chrome=IM_COL32(170,170,180,255);
 draw->AddLine(at(.26f,.87f),at(.22f,.99f),chrome,2*s);draw->AddLine(at(.74f,.87f),at(.78f,.99f),chrome,2*s);
 draw->AddLine(at(.24f,.94f),at(.76f,.94f),chrome,1.5f*s);
 if(!elided.empty()&&ImGui::IsMouseHoveringRect(p,ImVec2(p.x+w,p.y+h)))ImGui::SetTooltip("%s",elided.c_str());
 return pressed;
}
void DrawArcadeBackdrop(ImVec2 min,ImVec2 max) {
 auto* draw=ImGui::GetWindowDrawList();
 const float height=max.y-min.y,width=max.x-min.x,floor=min.y+height*.72f;
 draw->AddRectFilledMultiColor(min,ImVec2(max.x,floor),IM_COL32(30,8,26,255),IM_COL32(30,8,26,255),IM_COL32(12,6,16,255),IM_COL32(12,6,16,255));
 draw->AddRectFilledMultiColor(ImVec2(min.x,floor),max,IM_COL32(22,8,18,255),IM_COL32(22,8,18,255),IM_COL32(8,5,10,255),IM_COL32(8,5,10,255));
 // Neon signs glow on the back wall, and the floor catches them.
 const struct { float x,y,r; ImU32 color; } glows[]={{.15f,.18f,.20f,IM_COL32(230,40,60,9)},{.55f,.10f,.25f,IM_COL32(150,60,220,8)},{.88f,.22f,.18f,IM_COL32(230,60,40,9)}};
 for(const auto& glow:glows){
  const ImVec2 centre(min.x+width*glow.x,min.y+height*glow.y);
  for(int ring=16;ring>=1;--ring)draw->AddCircleFilled(centre,width*glow.r*ring/16,glow.color,48);
  draw->AddLine(ImVec2(centre.x-width*.04f,floor+height*.06f),ImVec2(centre.x+width*.04f,floor+height*.06f),(glow.color&0x00FFFFFF)|0x60000000,3*Scale());
 }
 draw->AddLine(ImVec2(min.x,floor),ImVec2(max.x,floor),IM_COL32(120,30,50,120),1.5f*Scale());
}
CardInteraction DrawMemberCard(const MemberCardView& card) {
 const float s=Scale();
 const auto p=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
 std::string elided;
 if(ImGui::IsRectVisible(p,ImVec2(p.x+width,p.y+58*s)))NoteUserText(card.name);
 const bool pressed=DrawCardButton(card.id,ImVec2(width,58*s),card.focused,card.focusChanged,card.disabled);
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
