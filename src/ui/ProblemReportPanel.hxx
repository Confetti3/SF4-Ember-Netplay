#pragma once
#include "GameMenu.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
// A sent report's status as Sent reports and the launch window name it, and
// what is known of how it went, then whether the small dump went with it.
inline const char* ReportStatusLabel(reports::Status status) {
    return loc::T(status == reports::Status::Received ? "reports.status.received" :
        status == reports::Status::Refused ? "reports.status.refused" :
        status == reports::Status::Pending ? "reports.status.unknown" : "reports.status.not_sent");
}
inline std::string ReportRecordText(const reports::Record& record) {
    std::string outcome = record.status == reports::Status::Pending ? loc::T("reports.interrupted") :
        record.status == reports::Status::Refused ? loc::T("reports.refused") :
        record.status == reports::Status::NotSent ? loc::T("reports.not_sent") : record.id.empty() ? loc::T("reports.received") :
        record.kind == "problem" ? loc::Tf("reports.sent", record.id) : loc::Tf("reports.received_id", record.id);
    return outcome + "\n\n" + loc::T(record.dump ? "reports.dump_included" : "reports.dump_not_included");
}
struct ReportIntent {
    platform::ServiceAction service = platform::ServiceAction::None;
    reports::Submission submission;
    bool close = false;
};
class ProblemReportPanel {
public:
    void Open(std::uint64_t preparation = 0);
    ReportIntent Draw(const platform::ServiceSnapshot& state, const std::string& error = {});
    std::vector<MenuEntry> Rows(const platform::ServiceSnapshot& state) const;
    MenuNavigation& Navigation() { return menu_.navigation; }
private:
    GameMenu menu_;
    reports::Submission submission_;
    std::uint64_t preparation_ = 0;
};
} }
