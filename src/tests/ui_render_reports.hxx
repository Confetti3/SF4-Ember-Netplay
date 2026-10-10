#pragma once
// Problem reports: the Problem reports settings screen and the sent reports,
// the report preview with its comment and small dump, and the launch window's
// offer after a crash, in every render locale.
#include "ui_render_support.hxx"
#include "../common/Localization.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/ProblemReportPanel.hxx"
#include "../ui/RecoveryMenu.hxx"
#include <algorithm>
#include <string>
#include <vector>

namespace {
inline std::vector<sf4e::reports::Record> SampleSentReports() {
    using namespace sf4e::reports;
    Record received;received.time=1791295380;received.status=Status::Received;received.id="3f9a0c1e5b7d42a8916e0d4c2b8f7a65";received.dump=true;
    Record refused;refused.time=1791381780;refused.status=Status::Refused;refused.reason="rate_limit";refused.kind="problem";
    Record lost;lost.time=1791468180;lost.status=Status::NotSent;lost.reason="no_connection";lost.automatic=true;
    Record cut;cut.time=1791554580;cut.status=Status::Pending;cut.automatic=true;
    return {received,refused,lost,cut};
}
inline auto FindRow(const std::vector<sf4e::ui::MenuEntry>& rows,const char* id) {
    return std::find_if(rows.begin(),rows.end(),[&](const sf4e::ui::MenuEntry& e){return e.id==id;});
}
// The launch window's message when it is not showing a crash.
const char* const RecoveryFolderMessage="The selected folder does not contain SSFIV.exe. Choose the installed game folder or close recovery without starting SF4.";
// Mode 8: the report preview alone, filling the viewport.
inline void DrawReportPreviewMode(sf4e::ui::ProblemReportPanel& panel,const sf4e::platform::ServiceSnapshot& state) {
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Report preview test",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNavInputs);
    panel.Draw(state);ImGui::End();
}
}

template<class Draw> void ShootReports(sf4e::ui::ProblemReportPanel& panel,sf4e::platform::ServiceSnapshot& state,Draw draw) {
    using namespace sf4e;
    reports::Report report;
    report.meta={"problem","1.2.3","stable",std::string(64,'a'),"abc123-dirty","Windows 10.0 build 26100"};
    report.logs={{"sf4e.log","Game started\nBearer [token]\nRoom connection lost\n"},{"launcher.log","Launcher started\n"}};
    report.minidump="MDMP"+std::string(100,'\0');
    state.reporting.preview=std::make_shared<const reports::Report>(report);
    state.reporting.phase=reports::Phase::Preview; state.reporting.message=loc::T("reports.preview_detail"); panel.Open();
    draw("report-preview");
    Require(panel.Navigation().Focus()=="report-close","Report preview default was not Don't send");
    auto rows=panel.Rows(state);
    const auto send=FindRow(rows,"report-send");
    Require(send!=rows.end()&&!send->enabled,"Problem report sent without a comment");
    // The small dump is off until ticked for this report.
    Require(FindRow(rows,"report-dump")!=rows.end()&&FindRow(rows,"report-dump")->value==loc::T("common.off"),"The small dump was on before it was ticked");
    panel.Navigation().Focus("report-sf4e.log",rows);draw(nullptr,ui::MenuInput::Select,1);draw("report-log-reader");
    Require(panel.Navigation().Reading(),"Report log did not use the shared reader");
    draw(nullptr,ui::MenuInput::Back,1);draw();
    // Exercise the shared editor through ImGui's keyboard event queue, never
    // synthesized menu Select. Both Enter keys must keep two-line drafts open.
    for(const auto enter:{ImGuiKey_Enter,ImGuiKey_KeypadEnter}) {
        panel.Navigation().Focus("report-comment",panel.Rows(state));draw(nullptr,ui::MenuInput::Select,1);draw();
        auto& io=ImGui::GetIO();
        io.AddInputCharactersUTF8("First line.");draw();
        panel.Navigation().EditAccepts(false);
        io.AddKeyEvent(enter,true);draw(nullptr,0,1);
        Require(panel.Navigation().Editing(),"Enter closed the multiline editor");
        Require(panel.Navigation().Draft()=="First line.\n","Enter did not insert a newline");
        io.AddKeyEvent(enter,false);draw();
        io.AddInputCharactersUTF8("Second line after Enter.");draw();
        const std::string comment="First line.\nSecond line after Enter.";
        Require(panel.Navigation().Editing()&&panel.Navigation().Draft()==comment,"Multiline editor lost subsequent text");
        // Send modifiers in their own frame so input trickling cannot delay
        // the commit chord. The highlighted Cancel must not override it.
        io.AddKeyEvent(ImGuiMod_Ctrl,true);draw(nullptr,0,1);
        io.AddKeyEvent(enter,true);draw(nullptr,0,1);
        Require(!panel.Navigation().Editing(),"Ctrl+Enter did not accept the multiline comment");
        const auto accepted=panel.Rows(state);
        const auto saved=FindRow(accepted,"report-comment");
        Require(saved!=accepted.end()&&saved->value==comment,"Keyboard acceptance lost the newline or second line");
        io.AddKeyEvent(enter,false);draw(nullptr,0,1);
        io.AddKeyEvent(ImGuiMod_Ctrl,false);draw();
        panel.Open();draw();
    }
    panel.Navigation().Focus("report-comment",panel.Rows(state));draw(nullptr,ui::MenuInput::Select,1);draw("report-comment-editor");
    Require(panel.Navigation().Editing(),"Report comment did not open the shared editor");
    // Type into the active shared editor: changing its model draft directly
    // would let ImGui's still-empty input buffer overwrite the comment.
    ImGui::GetIO().AddInputCharactersUTF8("The game stopped after joining a room.");draw();
    panel.Navigation().EditAccepts(true);draw(nullptr,ui::MenuInput::Select,1);draw();
    Require(!panel.Navigation().Editing(),"Report comment editor did not accept the comment");
    rows=panel.Rows(state);
    panel.Navigation().Focus("report-comment",rows);draw("report-problem-comment");
    const auto ready=FindRow(rows,"report-send");
    Require(ready!=rows.end()&&ready->enabled,"Report consent was unavailable after entering a comment");
    Require(ready->detail.find(loc::T("reports.dump_warning"))==std::string::npos,"Final confirmation warned of a dump that is not included");
    // Without the dump: the confirmation starts on Don't send.
    panel.Navigation().Focus("report-send",rows);draw(nullptr,ui::MenuInput::Select,1);draw("report-consent");
    Require(panel.Navigation().Confirming()&&!panel.Navigation().ConfirmSelected(),"Report consent default was Send");
    draw(nullptr,ui::MenuInput::Back,1);draw();
    // Ticked for this report: the row shows its size and the confirmation warns.
    panel.Navigation().Focus("report-dump",panel.Rows(state));draw(nullptr,ui::MenuInput::Right,1);draw();
    const auto included=panel.Rows(state);
    const auto withDump=FindRow(included,"report-send");
    Require(FindRow(included,"report-dump")->value!=loc::T("common.off")&&withDump->detail.find(loc::T("reports.dump_warning"))!=std::string::npos,
        "Ticking Small dump did not reach the confirmation");
    panel.Navigation().Focus("report-send",included);draw(nullptr,ui::MenuInput::Select,1);draw("report-consent-dump");
    Require(panel.Navigation().Confirming()&&!panel.Navigation().ConfirmSelected(),"Report consent with the dump default was Send");
    // A fresh confirm press declines; selecting Send earlier must not survive
    // a cancellation or an error/retry reopening of the confirmation.
    draw(nullptr,ui::MenuInput::Select,1);draw();
    Require(!panel.Navigation().Confirming(),"Default report consent did not decline");
    draw(nullptr,ui::MenuInput::Select,1);draw();
    draw(nullptr,ui::MenuInput::Right,1);draw();
    Require(panel.Navigation().ConfirmSelected(),"Report consent could not explicitly select Send");
    draw(nullptr,ui::MenuInput::Back,1);draw();
    // Left takes the dump out again.
    panel.Navigation().Focus("report-dump",panel.Rows(state));draw(nullptr,ui::MenuInput::Left,1);draw();
    const auto excluded=panel.Rows(state);
    Require(FindRow(excluded,"report-send")->detail.find(loc::T("reports.dump_warning"))==std::string::npos,"Excluded dump retained inclusion warning");
    state.reporting.phase=reports::Phase::Submitting;state.reporting.message=loc::T("reports.sending");draw("report-sending");
    state.reporting.phase=reports::Phase::Retry;state.reporting.retryAt=std::chrono::steady_clock::now()+std::chrono::seconds(600);draw("report-rate-limit");
    state.reporting.retryAt={};draw();
    panel.Navigation().Focus("report-send",panel.Rows(state));draw(nullptr,ui::MenuInput::Select,1);draw();
    Require(panel.Navigation().Confirming()&&!panel.Navigation().ConfirmSelected(),"Report retry consent default was Send");
    draw(nullptr,ui::MenuInput::Back,1);draw();
    state.reporting.phase=reports::Phase::Sent;state.reporting.id=std::string(32,'b');state.reporting.message=loc::Tf("reports.sent",state.reporting.id);draw("report-sent");
    panel.Open(state.reporting.preparation + 1);draw("report-preparing-again");
    Require(panel.Navigation().Focus()=="report-close","Reopened report preview default was not Don't send");
    const auto waiting=panel.Rows(state);
    Require(std::none_of(waiting.begin(),waiting.end(),[](const ui::MenuEntry& row){return row.id=="report-meta"||row.id=="report-copy";}),"Previous report leaked into a new preview");
    state={};
    report.meta.kind="crash";state.reporting.phase=reports::Phase::Preview;state.reporting.preview=std::make_shared<const reports::Report>(report);
    panel.Open();draw();
    Require(panel.Navigation().Focus()=="report-close","Crash report preview default was not Don't send");
    panel.Navigation().Focus("report-send",panel.Rows(state));draw(nullptr,ui::MenuInput::Select,1);draw();
    Require(panel.Navigation().Confirming()&&!panel.Navigation().ConfirmSelected(),"Crash report consent default was Send");
    draw(nullptr,ui::MenuInput::Select,1);draw();
    Require(!panel.Navigation().Confirming(),"Default crash report consent did not decline");
    state={};
}

// Settings, Problem reports: the setting, off by default, with what a report
// holds, and the sent reports, newest first.
template<class Draw,class Page>
void ShootProblemReports(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    using namespace sf4e;using namespace ui;
    std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    view.preferences.sendProblemReports=false;
    page("problem-reports");
    Require(rows.size()==2&&rows[0].id=="send-reports"&&rows[1].id=="sent-reports","Problem reports rows changed");
    Require(rows[0].value==loc::T("common.off"),"Problem reports are not off by default");
    Require(rows[0].detail.find(loc::T("reports.what_is_sent"))!=std::string::npos,"The setting no longer says what a report holds");
    Require(rows[0].reading&&rows[0].adjustable,"The setting cannot be both read in full and changed");
    // Select reads the whole text; Back returns to the row.
    draw(nullptr,MenuInput::Select,1);draw("problem-reports-reader");
    Require(shell.Navigation().Reading(),"Select did not open what a report holds");
    draw(nullptr,MenuInput::Back,1);draw();
    Require(!shell.Navigation().Reading()&&shell.Navigation().Screen()=="problem-reports","Back did not close the reader");
    view.services.sentReports=SampleSentReports();
    page("sent-reports");
    Require(rows.size()==4&&rows[0].id=="sent-3"&&rows[3].id=="sent-0","Sent reports are not newest first");
    Require(rows[0].value==loc::T("reports.status.unknown")&&rows[1].value==loc::T("reports.status.not_sent")&&rows[2].value==loc::T("reports.status.refused")&&
        rows[3].value==loc::T("reports.status.received"),"Sent reports lost their status");
    Require(rows[0].detail.find(loc::T("reports.interrupted"))!=std::string::npos,"An interrupted send does not say it may have arrived");
    Require(rows[3].detail.find("3f9a0c1e5b7d42a8916e0d4c2b8f7a65")!=std::string::npos,"A received report lost its ID");
    Require(rows[3].detail.find(loc::T("reports.dump_included"))!=std::string::npos&&rows[2].detail.find(loc::T("reports.dump_not_included"))!=std::string::npos,
        "A sent report does not say whether the dump went");
    Require(rows[2].detail.find(loc::T("reports.row"))!=std::string::npos&&rows[3].detail.find(loc::T("reports.crash_report"))!=std::string::npos,
        "A sent report does not say what kind it was");
    for(const auto& row:rows)Require(row.info,"A sent report row does something on Select");
    shell.Navigation().Focus("sent-0",rows);draw("sent-reports-received");
    // A record that cannot be read says so before the sends it still shows.
    view.services.sentReadable=false;
    page("sent-reports");draw("sent-reports-unreadable");
    Require(rows.size()==5&&rows[0].id=="sent-unreadable"&&rows[0].info,"An unreadable record is not named");
    view.services.sentReadable=true;
    view.services.sentReports.clear();
    page("sent-reports");draw("sent-reports-empty");
    Require(rows.size()==1&&rows[0].id=="sent-none","An empty list says nothing");
    SetMenuEntriesProbe({});
}
// Settings > Problem reports, then the preview on its own in mode 8.
template<class Draw,class Page>
void ShootReportScreens(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page,
    sf4e::ui::ProblemReportPanel& panel,sf4e::platform::ServiceSnapshot& state,int& mode) {
    ShootProblemReports(shell,view,draw,page);
    mode=8;ShootReports(panel,state,draw);mode=0;
}

// The launch window after a crash with reports off: the offer, Always send's
// confirmation with Cancel first, and the row while and once the report goes.
template<class Draw>
void ShootCrashReportOffer(sf4e::ui::GameMenu& menu,sf4e::platform::ServiceSnapshot& state,sf4e::ui::RecoveryReport& report,
    std::string& message,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    const std::string before=message;
    message=loc::T("launcher.game_crashed");state={};
    menu=GameMenu{};menu.navigation=RecoveryNavigation(false);
    report={};report.asking=true;
    std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    draw("crash-report-offer");
    Require(FindRow(rows,"report")!=rows.end()&&FindRow(rows,"report-always")!=rows.end(),"The crash message does not offer the report");
    Require(!FindRow(rows,"report")->confirm&&FindRow(rows,"report-always")->confirm,"Send this report should open its preview, and Always send ask first");
    Require(FindRow(rows,"report-dump")==rows.end(),"The crash message offers a dump outside the preview");
    // Always send's confirmation says what every report holds and where it goes.
    Require(FindRow(rows,"report-always")->detail.find(loc::T("reports.payload"))!=std::string::npos,"Always send does not say what is sent");
    Require(menu.navigation.Focus()!="report"&&menu.navigation.Focus()!="report-always","The offer took the focus");
    menu.navigation.Prefer("report-always");draw();
    draw(nullptr,MenuInput::Select,1);draw("crash-report-always-confirm");
    Require(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected(),"Always send's confirmation does not start on Cancel");
    draw(nullptr,MenuInput::Back,1);draw();
    Require(!menu.navigation.Confirming(),"Back did not cancel Always send");
    // Sent without a press: one row says it is on its way, then how it went.
    report={};report.sending=true;message=loc::T("launcher.game_crashed_sending");
    state.pending=true;state.lastAction=platform::ServiceAction::SendReport;state.message=loc::T("reports.sending");
    draw("crash-report-sending");
    Require(FindRow(rows,"report")==rows.end()&&FindRow(rows,"report-status")!=rows.end(),"The offer stayed while the report was sending");
    state.pending=false;report.sending=false;report.outcome=SampleSentReports().front();report.outcome->dump=false;report.outcome->automatic=true;
    state.succeeded=true;state.message=loc::Tf("reports.sent",report.outcome->id);
    draw("crash-report-sent");
    Require(FindRow(rows,"report-status")->value==loc::T("reports.status.received"),"The sent report's row lost its status");
    Require(FindRow(rows,"report-status")->detail.find(loc::T("reports.dump_not_included"))!=std::string::npos,"An automatic report does not say the dump stayed");
    SetMenuEntriesProbe({});
    report={};state={};message=before;menu=GameMenu{};menu.navigation=RecoveryNavigation(false);
}
