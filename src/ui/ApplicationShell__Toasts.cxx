#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "GameMenu.hxx"
#include <imgui_internal.h>
#include <algorithm>
#include <utility>

namespace sf4e { namespace ui {
namespace {
constexpr std::size_t ToastsKept=20;
std::string Ago(double seconds) {
 const int minutes=static_cast<int>(seconds/60);
 return minutes<1?std::string(loc::T("public.updated_now")):loc::Tf("public.updated_minutes",minutes);
}
// Each tone has its own shape, so colour is never the only cue (Toasts board):
// done a disc, waiting a triangle, a refusal a square, the rest a diamond.
void ToneMark(ImDrawList* d,ImVec2 c,float r,Tone tone,ImU32 ink) {
 switch(tone){
 case Tone::Success:d->AddCircleFilled(c,r,ink);break;
 case Tone::Pending:d->AddTriangleFilled({c.x,c.y-r},{c.x+r,c.y+r*.8f},{c.x-r,c.y+r*.8f},ink);break;
 case Tone::Error:d->AddRectFilled({c.x-r*.8f,c.y-r*.8f},{c.x+r*.8f,c.y+r*.8f},ink);break;
 default:d->AddQuadFilled({c.x,c.y-r},{c.x+r,c.y},{c.x,c.y+r},{c.x-r,c.y},ink);break;
 }
}
}

// A notice or refusal is taken once, when its text first shows; one said
// again after it lapsed is taken again. The cards never take a game button:
// over a match the newest one's buttons only show their keys. Over the menus
// a new action toast holds focus until another press or click (Draw), and
// the Notifications rows carry the same words and actions.
void ApplicationShell::DrawToasts(double now,bool overMatch,bool listed) {
 unsigned fresh=0;
 const auto take=[&](const std::string& text,Tone tone,double until,std::string& seen,ToastKind kind){
  if(text==seen)return;
  seen=text;
  if(text.empty())return;
  const bool extra=noticeExtra_.text==text&&&seen==&noticeSeen_;
  toasts_.push_front({++toastCount_,text,tone,now,(std::max)(until,now+4),extra?noticeExtra_.kind:kind,extra?noticeExtra_.actions:std::vector<ToastAction>{}});
  if(!toasts_.front().actions.empty())fresh=toastCount_;
  if(toasts_.size()>ToastsKept)toasts_.pop_back();
  // Speech reads a toast once, as it appears.
  SayMenuNotice(text);
 };
 take(notice_,noticeTone_,noticeUntil_,noticeSeen_,ToastKind::None);
 take(error_,Tone::Error,now+6,errorSeen_,ToastKind::System);
 if(listed){toastFocus_=0;return;}
 // A toast takes focus once, as it comes, and only over a free menu: nothing
 // open, nothing being typed. One that came over a match or a dialog waits
 // in its card and in Notifications.
 const auto& nav=menu_.navigation;
 const bool menuFree=nav.Mode()==MenuNavigation::Modal::None&&!menu_.NoticeOpen()&&!menu_.typesText;
 if(fresh&&!overMatch&&menuFree)toastFocus_=fresh;
 // Stacked at the board's right corner, newest on top: under the match HUD's
 // bars over a match, ending on the hint bar over the menus. Inside TV
 // mode's safe area, as the board is (ShellArea).
 const auto& c=v2::Colors();const v2::Box view=ShellArea();
 const float s=Scale(),side=v2::SideMargin(view.W(),s),w=(std::min)(420*s,view.W()-2*side);
 // A narrow screen has room for the newest only.
 const std::size_t most=v2::Narrow(view.W(),s)?1:3;
 const float size=17*s,icon=28*s,pad=14*s,x=view.max.x-side-w,button=40*s,keySize=11*s,labelSize=15*s;
 ImFont* font=HeadingFont();
 // A dialog open over the menus takes every click and key, so the buttons
 // wait for it to close.
 const bool modal=ImGui::GetTopMostPopupModal()!=nullptr;
 struct Shown { Toast* t; std::string time; float timeW,wrap,h; };
 std::vector<Shown> shown;
 for(auto& t:toasts_){
  if(now>=t.until)continue;
  if(shown.size()==most)break;
  Shown e{&t,Ago(now-t.at)};
  e.timeW=ImGui::GetFont()->CalcTextSizeA(12*s,FLT_MAX,0,e.time.c_str()).x;
  e.wrap=w-2*pad-icon-10*s-e.timeW-10*s;
  // Eyebrow, text and, on the newest only, the buttons row (Toasts board).
  float h=font->CalcTextSizeA(size,FLT_MAX,e.wrap,t.text.c_str()).y;
  if(t.kind!=ToastKind::None)h+=16*s;
  if(shown.empty()&&!t.actions.empty()&&!modal)h+=9*s+button;
  e.h=(std::max)(icon,h)+24*s;
  shown.push_back(std::move(e));
 }
 if(toastFocus_&&(shown.empty()||shown[0].t->id!=toastFocus_||overMatch))toastFocus_=0;
 float y=view.min.y+110*s;
 if(!overMatch){y=view.max.y-72*s;for(const auto& e:shown)y-=e.h+8*s;y+=8*s;}
 static const char* const kinds[]={"","toast.kind.invitation","toast.kind.room","toast.kind.update","toast.kind.match","toast.kind.system"};
 auto* d=ImGui::GetForegroundDrawList();
 for(const auto& e:shown){
  const Toast& t=*e.t;
  const ImVec2 a(x,y),b(x+w,y+e.h);
  const ImU32 tone=ImGui::ColorConvertFloat4ToU32(ToneColor(t.tone));
  d->AddRectFilled({a.x,a.y+8*s},{b.x,b.y+8*s},v2::color::Alpha(c.ink,.5f),4*s);
  d->AddRectFilled(a,b,v2::PanelGround(),4*s);
  d->AddRect(a,b,c.border,4*s,0,s);
  d->AddRectFilled(a,{a.x+4*s,b.y},tone,4*s,ImDrawFlags_RoundCornersLeft);
  if(t.id==toastFocus_&&!modal)v2::Focus(d,a,b);
  ToneMark(d,{a.x+pad+icon*.5f,a.y+12*s+icon*.5f},8*s,t.tone,tone);
  const float left=a.x+pad+icon+10*s;float line=a.y+12*s;
  if(t.kind!=ToastKind::None){
   d->AddText(font,13*s,{left,line},tone,loc::T(kinds[static_cast<int>(t.kind)]));line+=16*s;
  }else line+=(std::max)(0.f,(icon-size)*.5f);
  NoteUserText(t.text);
  d->AddText(font,size,{left,line},c.text,t.text.c_str(),nullptr,e.wrap);
  line+=font->CalcTextSizeA(size,FLT_MAX,e.wrap,t.text.c_str()).y+9*s;
  d->AddText(ImGui::GetFont(),12*s,{b.x-pad-e.timeW,a.y+14*s},c.muted,e.time.c_str());
  // The buttons, each with its key: Select the first, Back the last. Over the
  // menus a small window over the card takes the pointer from the list under it.
  if(&e==&shown[0]&&!t.actions.empty()&&!modal){
   if(!overMatch){
    ImGui::SetNextWindowPos(a);ImGui::SetNextWindowSize({w,e.h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::Begin("##toast",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
   }
   float bx=left;
   for(std::size_t i=0;i<t.actions.size();++i){
    const bool first=i==0;
    const char* key=first?MenuSelectGlyph():MenuBackGlyph();
    const float keyW=(std::max)(40*s,ImGui::GetFont()->CalcTextSizeA(keySize,FLT_MAX,0,key).x+12*s);
    const float labelW=font->CalcTextSizeA(labelSize,FLT_MAX,0,t.actions[i].label.c_str()).x;
    const ImVec2 p(bx,line),q(bx+12*s+keyW+8*s+labelW+12*s,line+button);
    bool hovered=false;
    if(!overMatch){
     ImGui::SetCursorScreenPos(p);ImGui::PushID(static_cast<int>(i));
     if(ImGui::InvisibleButton("##act",{q.x-p.x,button}))toastAsked_={t.id,i};
     hovered=ImGui::IsItemHovered();ImGui::PopID();
    }
    d->AddRectFilled(p,q,hovered?c.hover:first?c.button:v2::color::Alpha(c.button,0),4*s);
    d->AddRect(p,q,first?c.hover:c.border,4*s,0,s);
    const ImVec2 k(p.x+12*s,p.y+9*s);
    d->AddRectFilled(k,{k.x+keyW,k.y+22*s},c.ink,11*s);
    d->AddRect(k,{k.x+keyW,k.y+22*s},c.hover,11*s,0,s);
    d->AddText(ImGui::GetFont(),keySize,{k.x+(keyW-ImGui::GetFont()->CalcTextSizeA(keySize,FLT_MAX,0,key).x)*.5f,k.y+(22*s-keySize)*.5f},c.text,key);
    d->AddText(font,labelSize,{k.x+keyW+8*s,p.y+(button-labelSize)*.5f},first?c.text:c.muted,t.actions[i].label.c_str());
    bx=q.x+8*s;
   }
   if(!overMatch){ImGui::End();ImGui::PopStyleVar();}
  }
  // A click anywhere else gives focus back to the list.
  if(t.id==toastFocus_&&ImGui::IsMouseClicked(0)&&!ImGui::IsMouseHoveringRect(a,b))toastFocus_=0;
  y=b.y+8*s;
 }
}

// A toast's action, asked for by its buttons, its keys or its row: the toast
// goes, and the action opens its screen at its row.
void ApplicationShell::AnswerToast(double now) {
 const auto [id,index]=std::exchange(toastAsked_,{0,0});
 const auto t=std::find_if(toasts_.begin(),toasts_.end(),[&](const Toast& t){return t.id==id;});
 if(!id||t==toasts_.end()||t->actions.empty())return;
 const ToastAction act=t->actions[(std::min)(index,t->actions.size()-1)];
 t->until=(std::min)(t->until,now);
 if(toastFocus_==id)toastFocus_=0;
 if(act.screen.empty())return;
 auto& nav=menu_.navigation;
 if(nav.Screen()!=act.screen){
  // Tournament matches sits under Online play and Ember ID, so Back goes there.
  if(act.screen=="tournament-matches"){nav.Push("online");nav.Push("identity");}
  nav.Push(act.screen);
 }
 if(!act.row.empty())nav.Prefer(act.row);
}

// Each said toast, newest first; one with actions offers them at its row.
std::vector<MenuEntry> ApplicationShell::NotificationRows(double now,std::string& title) const {
 title=loc::T("home.notifications");
 std::vector<MenuEntry> rows;
 for(const auto& t:toasts_){
  // Its kind over the words, so a long notice reads whole in its detail.
  static const char* const kinds[]={"notice.title","toast.kind.invitation","toast.kind.room","toast.kind.update","toast.kind.match","toast.kind.system"};
  auto row=InfoRow("toast-"+std::to_string(t.id),loc::T(kinds[static_cast<int>(t.kind)]),Ago(now-t.at),{});
  row.detail=t.text;
  for(std::size_t i=0;i<t.actions.size();++i)row.choices.push_back({std::to_string(i),t.actions[i].label});
  if(!row.choices.empty()){row.info=false;row.popout=true;}
  rows.push_back(std::move(row));
 }
 if(rows.empty())rows.push_back(Row("toasts-none",loc::T("notifications.none"),loc::T("notifications.none_detail"),false));
 return rows;
}
} }
