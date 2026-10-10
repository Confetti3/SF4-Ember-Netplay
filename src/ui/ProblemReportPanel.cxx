#include "ProblemReportPanel.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <chrono>
#include <utility>

namespace sf4e { namespace ui {
void ProblemReportPanel::Open(std::uint64_t preparation) {
    preparation_ = preparation;
    submission_={}; menu_.navigation=MenuNavigation("report");
    menu_.navigation.Prefer("report-close");
    menu_.navigation.NeutralGate();
    menu_.fitStatus=true;
    menu_.rootName=loc::T("reports.title"); menu_.backHint=loc::T("reports.dont_send");
}
std::vector<MenuEntry> ProblemReportPanel::Rows(const platform::ServiceSnapshot& state) const {
    std::vector<MenuEntry> rows;
    const auto reading=[&](std::string id,std::string label,std::string detail,std::size_t bytes) {
        auto row=Row(std::move(id),std::move(label),std::move(detail)); row.reading=true;
        row.value=loc::Tf("reports.bytes",bytes); row.detailText=DetailText::Chat; rows.push_back(std::move(row));
    };
    const auto* report=state.reporting.preparation >= preparation_ && state.reporting.phase != reports::Phase::Preparing ? state.reporting.preview.get() : nullptr;
    if (report) {
        const auto meta=reports::MetaJson(report->meta,submission_.comment);
        reading("report-meta",loc::T("reports.metadata"),"meta\n"+meta.dump(2),meta.dump().size());
        for (const auto& log:report->logs) reading("report-"+log.name,log.name,log.text,log.text.size());
        if (!report->minidump.empty()) {
            rows.push_back(Value("report-dump",loc::T("reports.dump"),submission_.includeDump?loc::Tf("reports.bytes",report->minidump.size()):
                std::string(loc::T("common.off")),std::string("minidump: crash.dmp\n")+loc::T("reports.dump_detail"),!state.reporting.Busy()&&state.reporting.id.empty()));
        }
        if (state.reporting.id.empty()) {
            rows.push_back(TextRow("report-comment",loc::T("reports.comment"),submission_.comment,8000,!state.reporting.Busy()));
            rows.back().multiline=true;
            rows.back().detail=loc::T(report->meta.kind=="problem"?"reports.comment_required":"reports.comment_optional");
            const bool validComment=reports::CharacterCount(submission_.comment)<=2000 &&
                (report->meta.kind!="problem"||submission_.comment.find_first_not_of(" \t\r\n")!=std::string::npos);
            const bool retry=std::chrono::steady_clock::now()>=state.reporting.retryAt;
            const std::string consent = std::string(loc::T("reports.consent")) +
                (submission_.includeDump && !report->minidump.empty() ? std::string("\n\n") + loc::T("reports.dump_warning") : std::string());
            rows.push_back(ConfirmRow("report-send",loc::T("reports.send"),consent,!state.reporting.Busy()&&validComment&&retry));
            rows.back().cancelLabel=loc::T("reports.dont_send");
        }
    }
    const bool accepted=report&&!state.reporting.id.empty();
    if (accepted) rows.push_back(Row("report-copy",loc::T("reports.copy_id"),state.reporting.id));
    rows.push_back(Row("report-close",loc::T(accepted?"common.close":"reports.dont_send"),loc::T("reports.close_detail")));
    return rows;
}
ReportIntent ProblemReportPanel::Draw(const platform::ServiceSnapshot& state, const std::string& error) {
    const auto rows=Rows(state);
    const bool waiting=state.reporting.preparation < preparation_ || state.reporting.phase==reports::Phase::Preparing;
    const std::string status=!error.empty()?error:waiting?std::string(loc::T("reports.preparing")):state.reporting.retryAt>std::chrono::steady_clock::now()?
        loc::Tf("reports.rate_limited",std::chrono::duration_cast<std::chrono::seconds>(state.reporting.retryAt-std::chrono::steady_clock::now()).count()+1):state.reporting.message;
    const auto action=menu_.Draw(loc::T("reports.title"),rows,status.c_str(),{},1,{},{},0,70,true,
        !error.empty()?Tone::Error:waiting||state.reporting.Busy()?Tone::Pending:state.reporting.Succeeded()?Tone::Success:Tone::Error);
    ReportIntent intent;
    if (action.kind==MenuAction::TextAccepted&&action.id=="report-comment") submission_.comment=action.text;
    else if (action.kind==MenuAction::Adjust&&action.id=="report-dump") submission_.includeDump=action.delta>0;
    else if (action.kind==MenuAction::Activate&&action.id=="report-send") {
        intent.service=platform::ServiceAction::SendReport; intent.submission=submission_; intent.submission.preview=state.reporting.preview;
    } else if (action.kind==MenuAction::Activate&&action.id=="report-copy") ImGui::SetClipboardText(state.reporting.id.c_str());
    else if (action.kind==MenuAction::Close||(action.kind==MenuAction::Activate&&action.id=="report-close")) {
        intent.close=true; intent.service=platform::ServiceAction::CancelReport;
    }
    return intent;
}
} }
