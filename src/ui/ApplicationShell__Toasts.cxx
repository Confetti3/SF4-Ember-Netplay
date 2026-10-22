#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include <algorithm>

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
// again after it lapsed is taken again. The cards never take focus or input:
// the status line and the Notifications rows carry the same words.
void ApplicationShell::DrawToasts(double now,bool overMatch,bool listed) {
 const auto take=[&](const std::string& text,Tone tone,double until,std::string& seen){
  if(text==seen)return;
  seen=text;
  if(text.empty())return;
  toasts_.push_front({++toastCount_,text,tone,now,(std::max)(until,now+4)});
  if(toasts_.size()>ToastsKept)toasts_.pop_back();
  // Speech reads a toast once, as it appears.
  if(v2::On())SayMenuNotice(text);
 };
 take(notice_,noticeTone_,noticeUntil_,noticeSeen_);
 take(error_,Tone::Error,now+6,errorSeen_);
 if(!v2::On()||listed)return;
 // Stacked at the board's right corner: under the match HUD's bars over a
 // match, upwards from the hint bar over the menus.
 const auto& c=v2::Colors();const auto* view=ImGui::GetMainViewport();
 const float s=Scale(),side=v2::SideMargin(view->Size.x,s),w=(std::min)(420*s,view->Size.x-2*side);
 // A narrow screen has room for the newest only.
 const int most=v2::Narrow(view->Size.x,s)?1:3;
 const float size=17*s,icon=28*s,pad=14*s,x=view->Pos.x+view->Size.x-side-w;
 float y=overMatch?view->Pos.y+110*s:view->Pos.y+view->Size.y-72*s;
 auto* d=ImGui::GetForegroundDrawList();ImFont* font=HeadingFont();
 int shown=0;
 for(const auto& t:toasts_){
  if(now>=t.until)continue;
  if(++shown>most)break;
  const std::string time=Ago(now-t.at);
  const float timeW=ImGui::GetFont()->CalcTextSizeA(12*s,FLT_MAX,0,time.c_str()).x;
  const float wrap=w-2*pad-icon-10*s-timeW-10*s;
  const float h=(std::max)(icon,font->CalcTextSizeA(size,FLT_MAX,wrap,t.text.c_str()).y)+24*s;
  if(!overMatch)y-=h;
  const ImVec2 a(x,y),b(x+w,y+h);
  const ImU32 tone=ImGui::ColorConvertFloat4ToU32(ToneColor(t.tone));
  d->AddRectFilled({a.x,a.y+8*s},{b.x,b.y+8*s},v2::color::Alpha(c.ink,.5f),4*s);
  d->AddRectFilled(a,b,v2::color::Alpha(c.panel,.96f),4*s);
  d->AddRect(a,b,c.border,4*s,0,s);
  d->AddRectFilled(a,{a.x+4*s,b.y},tone,4*s,ImDrawFlags_RoundCornersLeft);
  ToneMark(d,{a.x+pad+icon*.5f,a.y+12*s+icon*.5f},8*s,t.tone,tone);
  NoteUserText(t.text);
  d->AddText(font,size,{a.x+pad+icon+10*s,a.y+12*s+(std::max)(0.f,(icon-size)*.5f)},c.text,t.text.c_str(),nullptr,wrap);
  d->AddText(ImGui::GetFont(),12*s,{b.x-pad-timeW,a.y+14*s},c.muted,time.c_str());
  y=overMatch?b.y+8*s:a.y-8*s;
 }
}

std::vector<MenuEntry> ApplicationShell::NotificationRows(double now,std::string& title) const {
 title=loc::T("home.notifications");
 std::vector<MenuEntry> rows;
 for(const auto& t:toasts_){rows.push_back(InfoRow("toast-"+std::to_string(t.id),t.text,Ago(now-t.at),{}));}
 if(rows.empty())rows.push_back(Row("toasts-none",loc::T("notifications.none"),loc::T("notifications.none_detail"),false));
 return rows;
}
} }
