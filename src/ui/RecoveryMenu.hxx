#pragma once
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "UpdateChannelPick.hxx"
#include "VersionLine.hxx"
#include "ProblemReportPanel.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace sf4e { namespace ui {
// Report opens the crash report's preview; AlwaysSend was confirmed.
enum class RecoveryChoice { None, Folder, Retry, CheckUpdates, Install, Cancel, Close, Channel, Report, AlwaysSend };
// The crash's report in the launch window: offered (asking), or, once a send
// was asked for, on its way (sending) and then how it went (outcome, or
// failure: what stopped it before anything was sent).
struct RecoveryReport {
    bool asking = false, sending = false;
    std::optional<reports::Record> outcome;
    std::string failure;
};
// The crash report's rows from the report worker's state: offered; on its
// way once the worker allowed it or the player sent it; then how it went.
inline RecoveryReport CrashReportView(const platform::ServiceSnapshot& state) {
    const auto& reporting=state.reporting;
    RecoveryReport view;
    if(reporting.offer)view.asking=true;
    else if(reporting.record)view.outcome=reporting.record;
    // Before the worker allows an automatic send there is nothing to show;
    // a send from the preview ends the offer when it starts.
    else if(reporting.automatic?reporting.authorized:reporting.preparation&&reporting.phase!=reports::Phase::Idle){
        if(reporting.Busy()||state.pending)view.sending=true;
        else view.failure=reporting.message;
    }
    return view;
}
// The root screen names the header's "< Back / ..." breadcrumb (MenuScreenLabel).
inline MenuNavigation RecoveryNavigation(bool updates) { return MenuNavigation(updates?"updates":"recovery"); }
// Each newly found update takes the highlight once, so Select installs it
// rather than checking again; offered remembers the version that already did.
inline void OfferFoundUpdate(GameMenu& menu,const platform::ServiceSnapshot& state,std::string& offered) {
    if(!state.update.ok||!state.update.updateAvailable||state.pending||state.update.latestVersion==offered)return;
    offered=state.update.latestVersion;menu.navigation.Prefer("install");
}
// V2's left column of the launch window (Recovery.dc.html): why it opened, or
// the latest service result; the focused row's words in a box; and while an
// update is fetched its bar and checks (Update.dc.html: found, verified,
// installed). The rows are the cards on the right.
inline void DrawRecoveryPane(const platform::ServiceSnapshot& state,const std::string& status,Tone tone,const MenuEntry* row) {
    const float s=Scale();
    const auto& c=v2::Colors();
    auto* d=ImGui::GetWindowDrawList();
    if(!status.empty())v2::Paragraph(status,18*s,tone==Tone::Success||tone==Tone::Pending?ImGui::ColorConvertFloat4ToU32(ToneColor(tone)):c.text);
    // A box of words under a muted heading, on the frame's ground.
    const auto box=[&](const char* id,const std::string& heading,const std::string& words,DetailText kind){
        ImGui::Dummy(ImVec2(0,8*s));
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::ColorConvertU32ToFloat4(c.frame));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16*s,12*s));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,4*s);
        ImGui::BeginChild(id,ImVec2(-1,0),ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding|ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNavInputs);
        NoteDetailText(words,kind);
        v2::Paragraph(heading,15*s,c.muted);
        v2::Paragraph(words,16*s,c.text);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);ImGui::PopStyleColor();
    };
    // A reader's row (the release notes) shows its first line; Select opens the rest.
    if(row&&!row->detail.empty())box("recovery-row",row->label,row->reading?row->detail.substr(0,row->detail.find('\n')):row->detail,row->detailText);
    // A found update's first notes, as the offer shows them; the What's new row reads them all.
    const auto& notes=state.update.releaseNotes;
    if(state.update.ok&&state.update.updateAvailable&&!state.pending&&!notes.empty()&&!(row&&row->reading)){
        std::size_t end=0;
        for(int line=0;line<4&&end!=std::string::npos;++line)end=notes.find('\n',end?end+1:0);
        box("recovery-notes",loc::T("updates.whats_new"),notes.substr(0,end),DetailText::Chat);
    }
    if(!state.pending||state.lastAction!=platform::ServiceAction::InstallUpdate)return;
    ImGui::Dummy(ImVec2(0,16*s));
    const float w=ImGui::GetContentRegionAvail().x,font=ImGui::GetFontSize();
    const auto at=ImGui::GetCursorScreenPos();
    const float done=state.stageTotal?(std::min)(1.f,float(state.stageDone)/state.stageTotal):-1;
    d->AddRectFilled(at,ImVec2(at.x+w,at.y+14*s),c.frame,4*s);
    // No total yet (extracting, a step just begun): a block runs along the bar.
    const float from=done<0?(w-w/4)*(.5f+.5f*std::sin(float(ImGui::GetTime())*2)):0,to=done<0?from+w/4:w*done;
    d->AddRectFilled(ImVec2(at.x+from,at.y),ImVec2(at.x+to,at.y+14*s),c.accent,4*s);
    d->AddRect(at,ImVec2(at.x+w,at.y+14*s),c.border,4*s,0,s);
    const std::string words=state.updateStage==launcher::UpdateStage::Downloading&&state.stageTotal?
        loc::Tf("updates.downloaded_of_mb",state.stageDone/1048576.0,state.stageTotal/1048576.0):state.message;
    d->PushClipRect(ImVec2(at.x,at.y+20*s),ImVec2(at.x+w-56*s,at.y+20*s+font),true);
    d->AddText(ImVec2(at.x,at.y+20*s),c.text,words.c_str());
    d->PopClipRect();
    if(done>=0){
        const std::string pct=std::to_string(int(done*100))+"%";
        d->AddText(ImVec2(at.x+w-ImGui::CalcTextSize(pct.c_str()).x,at.y+20*s),c.muted,pct.c_str());
    }
    // The checks: found, verified once past Verifying, installed when the updater has it.
    const bool ticks[]={state.update.updateAvailable,state.updateStage>launcher::UpdateStage::Verifying,state.installed};
    const char* names[]={loc::T("updates.step_found"),loc::T("updates.step_verified"),loc::T("updates.step_installed")};
    float x=at.x;const float y=at.y+28*s+font+font*.5f;
    for(int i=0;i<3;++i){
        const ImU32 tone=ticks[i]?ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Success)):c.muted;
        if(ticks[i])d->AddPolyline(std::array<ImVec2,3>{ImVec2(x,y),ImVec2(x+4*s,y+4*s),ImVec2(x+11*s,y-5*s)}.data(),3,tone,0,2.5f*s);
        else d->AddCircle(ImVec2(x+5*s,y),5*s,tone,0,1.5f*s);
        d->AddText(ImVec2(x+18*s,y-font*.5f),tone,names[i]);
        x+=18*s+ImGui::CalcTextSize(names[i]).x+20*s;
    }
    ImGui::Dummy(ImVec2(w,28*s+font*2));
}
// Rendering returns intent only. The launcher owns dialogs, services and exit.
// messageTone is the launcher message's own severity: a selected folder is
// good news, a launch failure is not. The status shows one of two messages: the
// launcher's (why recovery opened, the folder just picked) or the latest
// service result (an update check, a download). serviceNewer says which
// happened last, so a finished check does not bury a folder that was rejected
// afterwards, nor the failure that opened this window. With no launcher message
// the service result always shows.
// canStart adds a way into the game to the updater, which otherwise only
// closes: it answers RecoveryChoice::Retry like the launch window's Retry.
// The updater also shows the channel its checks use and the installed
// version, so an offered version can be read against it. With channelPick,
// Left and Right on the channel row pick a channel and Select asks to switch
// to it (RecoveryChoice::Channel, the pick in channelPick->picked); without
// one the row only informs. The release notes of the offered version open in
// a reader from the "What's new" row.
// `report` adds the crash report's rows after Retry: Send this report, which
// opens its preview, and Always send, confirmed with Cancel first; or one row
// saying how the report went.
inline RecoveryChoice DrawRecoveryMenu(GameMenu& menu,const platform::ServiceSnapshot& state,const std::string& message,bool updates,
    Tone messageTone=Tone::Error,bool canStart=false,bool serviceNewer=true,ChannelPick* channelPick=nullptr,const RecoveryReport& report={}) {
    if(channelPick)channelPick->Settle(state.channel,state.pending);
    std::vector<MenuEntry> rows;
    // A row that waits while the worker is busy: the status line and the bar
    // say with what, so its pane does not add "Unavailable".
    const auto waits=[&](MenuEntry row){row.quiet=state.pending;return row;};
    if(!updates){
        rows.push_back(waits(Row("folder",loc::T("recovery.choose_folder"),loc::T("recovery.choose_folder_detail"),!state.pending)));
        rows.push_back(waits(Row("retry",loc::T("recovery.retry"),state.pending?loc::T("recovery.retry_busy"):loc::T("recovery.retry_detail"),!state.pending)));
        if(report.asking){
            rows.push_back(waits(Row("report",loc::T("reports.send_once"),loc::T("reports.send_once_detail"),!state.pending)));
            // Its confirmation says what every report holds, where it goes and how often.
            rows.push_back(waits(ConfirmRow("report-always",loc::T("reports.send_always"),
                std::string(loc::T("reports.payload"))+"\n\n"+loc::Tf("reports.send_always_detail",reports::AutomaticPerDay),!state.pending)));
        }else if(report.sending||report.outcome||!report.failure.empty()){
            rows.push_back(InfoRow("report-status",loc::T("reports.crash_report"),
                report.outcome?ReportStatusLabel(report.outcome->status):report.sending?loc::T("reports.status.sending"):loc::T("reports.status.not_sent"),
                report.outcome?ReportRecordText(*report.outcome):report.sending?std::string(loc::T("reports.sending")):report.failure));
        }
    }
    // A found update comes first, named by its version: installing it is
    // what the player came here for.
    if(state.update.ok&&state.update.updateAvailable){
        const bool verified=state.update.expectedSha256.size()==64;
        std::string detail=verified?loc::T("updates.install_detail"):loc::T("updates.unverified");
        // Going back: the offer is older than what is installed.
        if(state.update.goesBack)detail=std::string(loc::T("updates.install_going_back"))+"\n\n"+detail;
        rows.push_back(waits(ConfirmRow("install",loc::T("updates.install"),detail,!state.pending&&verified)));
        rows.back().value=state.update.latestVersion;
    }
    // The offered (or current) release's notes, as plain text in a reader the
    // pad and keys scroll. They are GitHub text: only ever drawn as text.
    if(updates&&state.update.ok){
        // Plain text already, made once as the check read them.
        const auto& notes=state.update.releaseNotes;
        if(!notes.empty()){
            auto row=Row("notes",loc::T("updates.whats_new"),loc::Tf("updates.whats_new_detail",state.update.latestVersion)+"\n\n"+notes);
            row.reading=true;row.detailText=DetailText::Chat;rows.push_back(std::move(row));
        }
    }
    rows.push_back(waits(Row("check",loc::T("updates.check"),state.pending?loc::T("updates.busy"):loc::T("updates.check_detail"),!state.pending)));
    if(updates){
        rows.push_back(waits(ChannelRow(state.channel,channelPick,state.pending)));
        if(!state.installedVersion.empty())rows.push_back(InfoRow("installed",loc::T("updates.installed_version"),state.installedVersion,loc::T("updates.installed_version_detail")));
    }
    if(state.pending)rows.push_back(Row("cancel",loc::T("updates.cancel"),loc::T("updates.cancel_detail")));
    if(updates&&canStart)
        rows.push_back(waits(Row("retry",loc::T("updates.start_game"),state.pending?loc::T("recovery.retry_busy"):loc::T("updates.start_game_detail"),!state.pending)));
    rows.push_back(Row("close",loc::T("common.close"),state.pending?loc::T("recovery.close_cancels_detail"):
        loc::T(updates?"updates.close_detail":"recovery.close_detail")));
    // The launch window names the version in its corner, as Home does; the
    // updater has its own rows for it.
    menu.footerNote=updates?std::string():VersionLine(state.installedVersion,state.channel);
    // Back on the root closes the window, so the legend says so.
    menu.backHint=updates?loc::T("updates.back_close"):loc::T("recovery.back_close");
    const auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
    const std::string windowName=std::string(loc::T("recovery.window"))+"###EmberRecovery";
    ImGui::Begin(windowName.c_str(),nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
    const bool service=!state.message.empty()&&(serviceNewer||message.empty());
    const std::string& status=service?state.message:message;
    const Tone tone=service?(state.pending?Tone::Pending:state.succeeded?Tone::Success:Tone::Error):
        !message.empty()?messageTone:state.pending?Tone::Pending:Tone::Neutral;
    // The words and the update's progress on the left, the rows as cards on
    // the right with Retry or a found update marked as the thing to do. Too
    // narrow for the column, the status goes to the hint bar.
    const float s=Scale(),width=vp->Size.x;
    const bool column=!v2::Narrow(width,s)&&width-2*v2::SideMargin(width,s)>=820*s;
    menu.paneLeft=menu.customDetail=true;menu.backdrop=false;menu.listWidth=520.f;
    for(auto& row:rows)row.valueHighlighted=row.id=="install"||(row.id=="retry"&&!updates);
    const auto action=menu.Draw(updates?loc::T("updates.title"):loc::T("recovery.title"),rows,column?"":status.c_str(),[&](const std::string& id){
        const auto row=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
        DrawRecoveryPane(state,status,tone,row==rows.end()?nullptr:&*row);
    },1,{},{},false,100,tone);
    ImGui::End();
    if(action.kind==MenuAction::Close)return RecoveryChoice::Close;
    if(action.kind==MenuAction::Adjust&&action.id=="channel"&&channelPick&&!state.pending)channelPick->Step(state.channel,action.delta);
    if(action.kind!=MenuAction::Activate)return RecoveryChoice::None;
    if(action.id=="folder")return RecoveryChoice::Folder;
    if(action.id=="report")return RecoveryChoice::Report;
    if(action.id=="report-always")return RecoveryChoice::AlwaysSend;
    if(action.id=="retry")return RecoveryChoice::Retry;
    if(action.id=="check")return RecoveryChoice::CheckUpdates;
    if(action.id=="install")return RecoveryChoice::Install;
    if(action.id=="channel")return channelPick&&channelPick->Pending(state.channel)?RecoveryChoice::Channel:RecoveryChoice::None;
    if(action.id=="cancel")return RecoveryChoice::Cancel;
    if(action.id=="close")return RecoveryChoice::Close;
    return RecoveryChoice::None;
}
} }
