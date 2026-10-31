#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include "MenuDialogs.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <algorithm>
#include <sstream>
#include <utility>

namespace sf4e { namespace ui {
namespace {
// The release a News row is of ("news-<index>"), or null.
const launcher::ReleaseNews* NewsOf(const ShellView& v,const std::string& id) {
 const auto& news=v.services.update.news;
 const auto i=RowKey<std::size_t>(id,"news-");
 return i&&*i<news.size()?&news[*i]:nullptr;
}
const char* NewsKind(const launcher::ReleaseNews& n){return loc::T(n.stable?"news.kind.update":"news.tab.changelog");}
}
std::string ApplicationShell::InstalledVersion(const ShellView& v) { return v.services.installedVersion.empty() ? v.build : v.services.installedVersion; }
std::vector<MenuEntry> ApplicationShell::NewsRows(const ShellView& v,const std::string& screen,std::string& title) const {
 title=loc::T("home.news");
 std::vector<MenuEntry> rows;
 const auto& news=v.services.update.news;const auto& offer=v.services.launchOffer;
 for(std::size_t i=0;i<news.size();++i){
  const auto& n=news[i];
  if(screen!="news"&&n.stable!=(screen=="news-updates"))continue;
  // Each release reads in full; the notes are plain text already (PlainReleaseNotes).
  rows.push_back(Row("news-"+std::to_string(i),n.title,(n.date.empty()?n.tag:n.date+" \xC2\xB7 "+n.tag)+"\n\n"+n.notes));
  rows.back().reading=true;
  // The card says the kind and whether it is newer than this PC's version; Update now follows the offered release.
  rows.back().value=std::string(NewsKind(n))+(n.newer?std::string(" \xC2\xB7 ")+loc::T("news.new"):"");rows.back().height=84;
  if(offer&&offer->tag==n.tag)rows.push_back(ConfirmRow("news-update-now",loc::T("updates.launch_now"),loc::Tf("news.update_now_detail",n.title),v.canEditPreferences&&!v.services.pending));
 }
 if(rows.empty())rows.push_back(Row("news-none",loc::T("news.none"),loc::T("news.none_detail"),false));
 return rows;
}
// All, then a tab for each kind of release the list has.
void ApplicationShell::NewsTabs(const ShellView& v,const std::string& screen) {
 const auto& news=v.services.update.news;
 const auto any=[&](bool stable){return std::any_of(news.begin(),news.end(),[&](const launcher::ReleaseNews& n){return n.stable==stable;});};
 menu_.tabs={{"news",loc::T("news.tab.all")}};
 if(any(true))menu_.tabs.push_back({"news-updates",loc::T("news.tab.updates")});
 if(any(false))menu_.tabs.push_back({"news-changelog",loc::T("news.tab.changelog")});
 for(int i=0;i<static_cast<int>(menu_.tabs.size());++i)if(menu_.tabs[i].first==screen){menu_.tab=i;menu_.eyebrow=std::string(loc::T("home.news"))+" \xC2\xB7 "+menu_.tabs[i].second;}
}
// News: its cards and the focused release's pane, under its tabs.
class ApplicationShell::NewsScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return NewsScreen(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override { return f.shell.NewsRows(f.view, f.screen, title); }
    GameMenu::Card Card(const ScreenFrame& f, double) const override {
        const ApplicationShell* s = &f.shell; const ShellView* v = &f.view;
        return [s, v](const MenuEntry& e, ImVec2 min, ImVec2 max) { return s->DrawNewsCard(*v, e, min, max); };
    }
    GameMenu::Detail Detail(const ScreenFrame& f) const override {
        ApplicationShell* s = &f.shell; const ShellView* v = &f.view; const std::vector<MenuEntry>* rows = f.rows;
        return [s, v, rows](const std::string& id) { s->DrawNewsDetail(*v, id, *rows); };
    }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        f.shell.NewsTabs(f.view, f.screen);
        const std::string& focus = menu.navigation.Focus();
        menu.customDetail = focus.compare(0, 5, "news-") == 0 && focus != "news-update-now" && focus != "news-none";
        menu.listWidth = 420;
    }
};
const ScreenPanel& ApplicationShell::NewsOwner() {
    static const NewsScreens news{};
    return news;
}
bool ApplicationShell::DrawNewsCard(const ShellView& v,const MenuEntry& e,ImVec2 min,ImVec2 max) const {
 const auto* n=NewsOf(v,e.id);
 if(!n)return false;
 auto* d=ImGui::GetWindowDrawList();const float s=Scale();const auto& c=v2::Colors();
 const float x=min.x+14*s,right=max.x-14*s,top=min.y+12*s;
 v2::Chip(d,ImVec2(x,top),NewsKind(*n),n->stable?c.accent:c.muted);
 float at=right-ImGui::CalcTextSize(n->date.c_str()).x;
 d->AddText(ImVec2(at,top+2*s),c.muted,n->date.c_str());
 if(n->newer){const char* fresh=loc::T("news.new");at-=ImGui::CalcTextSize(fresh).x+10*s;d->AddText(ImVec2(at,top+2*s),c.accent,fresh);}
 ImGui::PushFont(HeadingFont());
 NoteUserText(n->title);
 d->AddText(ImVec2(x,max.y-12*s-ImGui::GetFontSize()),c.text,FitLabel(n->title,right-x).c_str());
 ImGui::PopFont();
 return true;
}
void ApplicationShell::DrawNewsDetail(const ShellView& v,const std::string& id,const std::vector<MenuEntry>& rows) {
 const auto* n=NewsOf(v,id);
 if(!n)return;
 auto* d=ImGui::GetWindowDrawList();const float s=Scale();const auto& c=v2::Colors();
 // The hero band across the pane's top: the kind's chip and the title over the kind's ground.
 const ImVec2 a=ImGui::GetWindowPos(),b(a.x+ImGui::GetWindowWidth(),a.y+150*s);
 d->PushClipRect(a,b,false);
 const ImU32 far=n->stable?v2::color::Alpha(c.accent,.4f):c.header;
 d->AddRectFilledMultiColor(a,b,c.header,far,far,c.header);
 d->AddLine(ImVec2(a.x,b.y-s),ImVec2(b.x,b.y-s),c.border,s);
 d->PopClipRect();
 ImGui::PushFont(HeadingFont());
 const float title=ImGui::GetFontSize()*1.4f;
 NoteUserText(n->title);
 d->AddText(HeadingFont(),title,ImVec2(a.x+24*s,b.y-18*s-title),c.text,n->title.c_str());
 ImGui::PopFont();
 v2::Chip(d,ImVec2(a.x+24*s,b.y-28*s-title-ImGui::GetFontSize()),NewsKind(*n),n->stable?c.accent:c.muted);
 ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x+8*s,b.y+16*s));
 ImGui::TextDisabled("%s",(n->date.empty()?n->tag:n->date+" \xC2\xB7 "+n->tag).c_str());
 ImGui::Dummy(ImVec2(0,4*s));
 // The notes a line each, with the accent's mark; their own list marks give way to it.
 std::istringstream notes(n->notes);
 for(std::string line;std::getline(notes,line);){
  line.erase(0,line.find_first_not_of(" \t-*"));
  if(line.empty())continue;
  NoteUserText(line);
  const auto p=ImGui::GetCursorScreenPos();
  d->AddRectFilled(ImVec2(p.x,p.y+ImGui::GetFontSize()*.35f),ImVec2(p.x+6*s,p.y+ImGui::GetFontSize()*.35f+6*s),c.accent);
  ImGui::Indent(18*s);v2::Wrapped(line);ImGui::Unindent(18*s);
 }
 // Update now on the offered release, at the pane's foot: the same as its row, which asks first.
 if(!v.services.launchOffer||v.services.launchOffer->tag!=n->tag)return;
 const float button=52*s,foot=a.y+ImGui::GetWindowHeight()-16*s-button;
 if(ImGui::GetCursorScreenPos().y<foot)ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,foot));
 ImGui::PushStyleColor(ImGuiCol_Button,ImGui::ColorConvertU32ToFloat4(c.accent));
 ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImGui::ColorConvertU32ToFloat4(c.accentLight));
 ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(c.ink));
 ImGui::PushFont(HeadingFont());
 if(ImGui::Button(loc::T("updates.launch_now"),ImVec2(0,button))){menu_.navigation.Focus("news-update-now",rows);menu_.navigation.Choose(rows);}
 ImGui::PopFont();
 ImGui::PopStyleColor(3);
}
// The first Home asks the service for the launch check; the service's policy
// decides whether one is due, and the question is put up only once its offer
// exists and the player is free to answer it. Ask again until the service has
// taken the request: it declines to start while another operation runs.
void ApplicationShell::UpdateLaunchCheck(const ShellView& v,double now,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 if(!launchCheckSeeded_){launchCheck_=v.checkUpdatesAtLaunch;launchCheckSeeded_=true;}
 const bool home=nav.Screen()=="home";
 if(home&&v.services.launchCheck==platform::LaunchCheck::Waiting&&now-launchCheckAskedAt_>=1){
  launchCheckAskedAt_=now;
  ShellAction ask;ask.service=platform::ServiceAction::LaunchUpdateCheck;ask.command.generation=v.session.generation;
  submit(std::move(ask));
 }
 // Update now closes the game when the Launcher starts; when it does not, the player is still here and is told.
 const auto& services=v.services;
 if(services.lastAction==platform::ServiceAction::UpdateNow&&!services.pending&&!services.succeeded&&!services.message.empty()){
  if(!updateNowReported_){updateNowReported_=true;error_=services.message;}
 }else if(services.lastAction!=platform::ServiceAction::UpdateNow||services.pending)updateNowReported_=false;
 skipFailuresSeen_=v.services.skipFailures;
 // A skip that was refused or could not be saved is said on Home, and its offer is asked about again.
 if(home)if(const auto failure=updatePrompt_.Resolve(v.services))error_=*failure;
 UpdatePrompt::Place place;
 place.home=home;
 // canEditPreferences is the runtime's own test for a game that may be left (UpdateNow needs it).
 place.free=v.canEditPreferences&&v.session.room==RoomState::Idle&&v.session.match==MatchState::None&&!v.practiceFight&&!v.watchedGame.Active()&&
  v.inputCapture==input::Capture::Idle;
 place.uncovered=!nav.Editing()&&!nav.Reading()&&!nav.Confirming()&&!nav.Choosing()&&!menu_.NoticeOpen();
 place.settingsNoticeDone=gameSettingsChecked_||!v.showGameSettingsCard;
 if(const auto offer=updatePrompt_.Due(v.services,place)){
  updateGeneration_=v.session.generation;
  updatePrompt_.Asked(*offer);
  menu_.ShowNotice(UpdatePrompt::Dialog(*offer,v.services.installedVersion.empty()?v.build:v.services.installedVersion));
 }
 // A new offer is a toast too, News' Update now a press away.
 if(v.services.launchOffer&&v.services.launchOffer->tag!=updateToastTag_){
  updateToastTag_=v.services.launchOffer->tag;
  Say(loc::Tf("updates.ready_notice",updateToastTag_),Tone::Neutral,20,ToastKind::Update,{{loc::T("updates.launch_now"),"news","news-update-now"},{loc::T("updates.launch_later")}});
 }
}
// The question closed: Update now and Skip this version send the offer the
// player was shown; anything else leaves it for the next launch. A skip that
// could not be queued, or that the service reports unsaved, is not final: the
// offer is asked about again until it is saved.
void ApplicationShell::FinishUpdatePrompt(NoticeResult result,const Submit& submit) {
 const auto offer=updatePrompt_.Close();
 if(!offer)return;
 const auto answer=UpdatePrompt::AnswerOf(result);
 if(answer==UpdatePrompt::Answer::Later){updatePrompt_.Settle(*offer);return;}
 ShellAction r;r.command.generation=updateGeneration_;
 r.service=answer==UpdatePrompt::Answer::UpdateNow?platform::ServiceAction::UpdateNow:platform::ServiceAction::SkipUpdate;
 r.updateOffer=*offer;
 const bool skip=answer==UpdatePrompt::Answer::Skip;
 if(!submit(std::move(r))){error_=loc::T("error.queue_failed");if(!skip)updatePrompt_.Settle(*offer);return;}
 if(skip)updatePrompt_.Skipping(*offer,skipFailuresSeen_);else updatePrompt_.Settle(*offer);
}
void ApplicationShell::SetLaunchCheck(bool enabled) {
 if(enabled==launchCheck_&&launchCheckSaveError_.empty())return;
 std::string diagnostic;
 const bool saved=updateCheckSaver_?updateCheckSaver_(enabled,diagnostic):platform::SaveCheckUpdatesAtLaunch(enabled,diagnostic);
 if(saved){launchCheck_=enabled;launchCheckSaveError_.clear();}
 else launchCheckSaveError_=loc::T("common.save_failed");
}
} }
