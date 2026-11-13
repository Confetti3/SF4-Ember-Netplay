#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include "../common/Localization.hxx"
#include <imgui_internal.h>
#include <algorithm>
#include <utility>

namespace sf4e { namespace ui {
namespace {
// A card stays up ToastSeconds, or ActionToastSeconds with buttons
// (Notifications.hxx). Its words stay in Notifications either way.
std::string Ago(double seconds) {
 const int minutes=static_cast<int>(seconds/60);
 return minutes<1?std::string(loc::T("public.updated_now")):loc::Tf("public.updated_minutes",minutes);
}
// A toast's words: the first line says it, the rest (a request's "Played you 6 games", an invite's room) sits
// under it, quieter. Both wrap to `wrap`; the height is what they take, and DrawToastWords draws them.
struct Words { std::string head, tail; };
Words WordsOf(const std::string& text) {
 const auto cut=text.find('\n');
 return cut==std::string::npos?Words{text,{}}:Words{text.substr(0,cut),text.substr(cut+1)};
}
float WordsHeight(ImFont* font,const std::string& text,float wrap,float size,float s) {
 const auto words=WordsOf(text);
 float h=font->CalcTextSizeA(size,FLT_MAX,wrap,words.head.c_str()).y;
 if(!words.tail.empty())h+=4*s+ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,wrap,words.tail.c_str()).y;
 return h;
}
float DrawToastWords(ImDrawList* d,ImFont* font,ImVec2 at,float wrap,const std::string& text,float size,float s,ImU32 ink,ImU32 quiet) {
 const auto words=WordsOf(text);
 d->AddText(font,size,at,ink,words.head.c_str(),nullptr,wrap);
 float h=font->CalcTextSizeA(size,FLT_MAX,wrap,words.head.c_str()).y;
 if(!words.tail.empty()){
  d->AddText(ImGui::GetFont(),14*s,{at.x,at.y+h+4*s},quiet,words.tail.c_str(),nullptr,wrap);
  h+=4*s+ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,wrap,words.tail.c_str()).y;
 }
 return h;
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

void ApplicationShell::Tell(Notifications::Item item,double seconds) {
 const double now=ImGui::GetTime();
 notice_=item.text;noticeTone_=item.tone;noticeUntil_=now+seconds;
 // The status line now says what is told here: it is this toast, not another.
 noticeSeen_=notice_;
 notices_.Say(std::move(item),now);
}

// A notice or refusal on the status line is told once, when its text first
// shows; one said again after it lapsed is told again. Every card that came
// up is read out whether or not Ember shows, once as it appears (again if it
// is shown again). Returns the newest new card with buttons, or 0.
unsigned ApplicationShell::TakeNotices(double now) {
 const auto take=[&](const std::string& text,Tone tone,std::string& seen,ToastKind kind){
  if(text==seen)return;
  seen=text;
  if(text.empty())return;
  Notifications::Item item;item.text=text;item.tone=tone;item.kind=kind;
  notices_.Say(std::move(item),now);
 };
 take(notice_,noticeTone_,noticeSeen_,ToastKind::None);
 take(error_,Tone::Error,errorSeen_,ToastKind::System);
 unsigned fresh=0;
 for(const unsigned id:notices_.TakeAppeared())
  if(const auto* item=notices_.Find(id)){SayMenuNotice(item->text);if(!item->actions.empty())fresh=id;}
 return fresh;
}

// The cards, over Ember's menus only: while Ember is hidden the game is on
// screen and no card covers it (Background takes the notices alone). Over
// the menus a new action toast holds focus until another press or click
// (Draw), and the Notifications rows carry the same words and actions.
void ApplicationShell::DrawToasts(double now,bool listed,bool focusable) {
 const unsigned fresh=TakeNotices(now);
 if(listed){toastFocus_=0;return;}
 // A toast takes focus once, as it comes, and only over the shell's own free
 // menu: nothing open, nothing being typed. One that came over a dialog or
 // an embedded screen (fighter select, a report) waits in its card and in
 // Notifications.
 const auto& nav=menu_.navigation;
 const bool menuFree=focusable&&nav.Mode()==MenuNavigation::Modal::None&&!menu_.NoticeOpen()&&!menu_.typesText;
 if(fresh&&menuFree)toastFocus_=fresh;
 // Stacked at the board's right corner, newest on top, ending on the hint
 // bar. Inside TV mode's safe area, as the board is (ShellArea).
 const auto& c=v2::Colors();const v2::Box view=ShellArea();
 const float s=Scale(),side=v2::SideMargin(view.W(),s),w=(std::min)(420*s,view.W()-2*side);
 // A narrow screen has room for the newest only.
 const std::size_t most=v2::Narrow(view.W(),s)?1:3;
 const float size=17*s,icon=28*s,pad=14*s,x=view.max.x-side-w,button=40*s,keySize=26*s,labelSize=15*s;
 ImFont* font=HeadingFont();
 // A dialog open over the menus takes every click and key, so the buttons
 // wait for it to close.
 const bool modal=ImGui::GetTopMostPopupModal()!=nullptr;
 struct Shown { const Notifications::Item* t; std::string time; float timeW,wrap,h; };
 std::vector<Shown> shown;
 for(const auto* card:notices_.Cards(now)){
  const auto& t=*card;
  if(shown.size()==most)break;
  Shown e{card,Ago(now-t.at)};
  e.timeW=ImGui::GetFont()->CalcTextSizeA(12*s,FLT_MAX,0,e.time.c_str()).x;
  e.wrap=w-2*pad-icon-10*s-e.timeW-10*s;
  // Eyebrow, text and, on the newest only, the buttons row (Toasts board).
  float h=WordsHeight(font,t.text,e.wrap,size,s);
  if(t.kind!=ToastKind::None)h+=16*s;
  if(shown.empty()&&!t.actions.empty()&&!modal)h+=9*s+button;
  e.h=(std::max)(icon,h)+24*s;
  shown.push_back(std::move(e));
 }
 if(toastFocus_&&(shown.empty()||shown[0].t->id!=toastFocus_))toastFocus_=0;
 float y=view.max.y-72*s;for(const auto& e:shown)y-=e.h+8*s;y+=8*s;
 static const char* const kinds[]={"","toast.kind.invitation","toast.kind.room","toast.kind.update","toast.kind.match","toast.kind.system"};
 auto* d=ImGui::GetForegroundDrawList();
 for(const auto& e:shown){
  const auto& t=*e.t;
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
  line+=DrawToastWords(d,font,{left,line},e.wrap,t.text,size,s,c.text,c.muted)+9*s;
  d->AddText(ImGui::GetFont(),12*s,{b.x-pad-e.timeW,a.y+14*s},c.muted,e.time.c_str());
  // The buttons. A small window over the card takes the pointer from the list
  // under it. Each carries its key (Select the first, Back the last) only
  // while the toast holds focus and so takes those keys: over the post-match
  // prompt or a dialog they answer what is under it, and the card is clicked.
  if(&e==&shown[0]&&t.Answerable()&&!modal){
   ImGui::SetNextWindowPos(a);ImGui::SetNextWindowSize({w,e.h});
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
   ImGui::Begin("##toast",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing);
   ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
   float bx=left;
   const bool keyed=t.id==toastFocus_;
   for(std::size_t i=0;i<t.actions.size();++i){
    const bool first=i==0;
    // The key's prompt glyph, as the hint bar and the post-match buttons draw it.
    const char* key=!keyed?nullptr:first?MenuSelectGlyph():MenuBackGlyph();
    const float labelW=font->CalcTextSizeA(labelSize,FLT_MAX,0,t.actions[i].label.c_str()).x;
    const float mark=key?keySize+8*s:0;
    const ImVec2 p(bx,line),q(bx+12*s+mark+labelW+12*s,line+button);
    ImGui::SetCursorScreenPos(p);ImGui::PushID(static_cast<int>(i));
    if(ImGui::InvisibleButton("##act",{q.x-p.x,button}))toastAsked_={t.id,i};
    const bool hovered=ImGui::IsItemHovered();ImGui::PopID();
    d->AddRectFilled(p,q,hovered?c.hover:first?c.button:v2::color::Alpha(c.button,0),4*s);
    d->AddRect(p,q,first?c.hover:c.border,4*s,0,s);
    const ImVec2 k(p.x+12*s,p.y+(button-keySize)*.5f);
    if(key)DrawPromptGlyph(d,key,k,keySize,MenuArt(),s);
    d->AddText(font,labelSize,{k.x+mark,p.y+(button-labelSize)*.5f},first?c.text:c.muted,t.actions[i].label.c_str());
    bx=q.x+8*s;
   }
   ImGui::End();ImGui::PopStyleVar();
  }
  // A click anywhere else gives focus back to the list.
  if(t.id==toastFocus_&&ImGui::IsMouseClicked(0)&&!ImGui::IsMouseHoveringRect(a,b))toastFocus_=0;
  y=b.y+8*s;
 }
}

// A toast's action, asked for by its buttons, its keys or its row: it is
// answered (its card goes), and what it asks is done. A source's item is done
// by its source, as its own row would do it; a one-off's commands are sent as
// they are, in order, whatever the screen, and the action opens its screen at its row.
void ApplicationShell::AnswerToast(const ShellView& v,double now,const Submit& submit) {
 const auto [id,asked]=std::exchange(toastAsked_,{0,0});
 const auto* item=id?notices_.Answer(id,now):nullptr;
 if(!item)return;
 const std::size_t index=(std::min)(asked,item->actions.size()-1);
 const ToastAction act=item->actions[index];
 if(toastFocus_==id)toastFocus_=0;
 if(item->source==FriendsPanel::Source){friends_.AnswerNotice(v,item->key,index,submit);return;}
 for(const auto& command:act.commands){
  if(!submit(std::visit([](const auto& typed)->ShellAction{return typed;},command))){error_=loc::T("error.queue_failed");break;}
 }
 if(act.screen.empty())return;
 auto& nav=menu_.navigation;
 if(nav.Screen()!=act.screen){
  // Tournament matches sits under Online play and Ember ID, so Back goes there.
  if(act.screen=="tournament-matches"){nav.Push("online");nav.Push("identity");}
  if(NewsFamily(nav.Screen())&&NewsFamily(act.screen))nav.Replace(act.screen);
  else nav.Push(act.screen);
 }
 if(!act.row.empty())nav.Prefer(act.row);
}

// Notifications: the friends' requests and invites waiting, then the toasts
// said lately. A request's or invite's row is the Friends panel's own (A, More
// and its questions, answered as on the Friends tab); a toast's action chosen
// at its row is done as its toast's button would (AnswerToast). The bell that
// leads here is not shown on it.
class ApplicationShell::NotificationScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return screen == FriendsPanel::NotificationsScreen; }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override { return f.shell.NotificationRows(f.view, ImGui::GetTime(), title); }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        return friends_panel::NotificationRow(a.id) && f.shell.friends_.Activate(f.view, a, f.shell.menu_.navigation, submit);
    }
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        if (friends_panel::NotificationRow(a.id)) {
            const bool taken = f.shell.friends_.Choose(f.view, a, submit);
            f.shell.OpenFriendsPlayer();
            return taken;
        }
        const auto toast = RowKey<unsigned>(a.id, "toast-");
        const auto action = RowKey<std::size_t>(a.text, "");
        if (!toast || !action) return false;
        f.shell.toastAsked_ = {*toast, *action};
        return true;
    }
    // Y opens More on a request's or invite's row, before a room's own Y; X and Delete stay the room's.
    bool TakesShortcut(const ScreenFrame& f, const MenuAction& a) const override {
        return FriendsPanel::TakesOnNotifications(f.shell.menu_.navigation.Focus(), a.delta);
    }
    void Shortcut(const ScreenFrame& f, const MenuAction& a) const override { f.shell.friends_.NotificationShortcut(a, f.shell.menu_.navigation); }
    // Notifications is the first tab of the News strip, so it has no bell.
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        f.shell.NewsTabs(f.view, f.screen);
        menu.statusLeads.clear();
        if (f.rows) f.shell.friends_.DressNotifications(*f.rows, menu);
    }
};
const ScreenPanel& ApplicationShell::NotificationsOwner() {
    static const NotificationScreens notifications{};
    return notifications;
}

// What waits first, each as its source's own row, then each toast said, newest
// first; one whose actions nobody took offers them at its row.
std::vector<MenuEntry> ApplicationShell::NotificationRows(const ShellView& v,double now,std::string& title) {
 title=loc::T("home.notifications");
 std::vector<MenuEntry> rows;
 for(const auto* t:notices_.Listed()){
  if(t->Sourced()){if(auto row=friends_.RowOf(v,t->key))rows.push_back(std::move(*row));continue;}
  // Its kind over the words, so a long notice reads whole in its detail.
  static const char* const kinds[]={"notice.title","toast.kind.invitation","toast.kind.room","toast.kind.update","toast.kind.match","toast.kind.system"};
  auto row=InfoRow("toast-"+std::to_string(t->id),loc::T(kinds[static_cast<int>(t->kind)]),Ago(now-t->at),{});
  row.detail=t->text;
  if(t->Answerable())for(std::size_t i=0;i<t->actions.size();++i)row.choices.push_back({std::to_string(i),t->actions[i].label});
  if(!row.choices.empty()){row.info=false;row.popout=true;}
  rows.push_back(std::move(row));
 }
 friends_.AttachNotifications(rows,v,menu_.navigation.Choosing());
 // None yet: a row that says so and what will come, as Sent reports' empty list does.
 if(rows.empty())rows.push_back(InfoRow("toasts-none",loc::T("notifications.none"),"",loc::T("notifications.none_detail")));
 return rows;
}
} }
