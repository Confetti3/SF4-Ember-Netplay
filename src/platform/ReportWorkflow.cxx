#include "ReportWorkflow.hxx"
#include "Utf8.hxx"
#include "../common/install_paths.hxx"
#include "../common/Localization.hxx"
#include "../netplay/BoolPreferences.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../launcher/update/PackageInstaller.hxx"
#include "BuildIdentity.hxx"
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <winhttp.h>
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace sf4e { namespace reports {
namespace {
const char* SendReportsKey() {
    for (const auto& preference : netplay::BoolPreferences)
        if (preference.member == &netplay::PlayerPreferences::sendProblemReports) return preference.key;
    return "";
}
// The launcher settings, tried again while another process holds them, until
// `wait` is spent or the work is cancelled.
std::optional<nlohmann::json> LoadSettings(const std::wstring& directory, const std::function<bool()>& cancelled,
    std::chrono::milliseconds wait) {
    if (directory.empty()) return std::nullopt;
    const auto until = std::chrono::steady_clock::now() + wait;
    for (;;) {
        nlohmann::json settings; std::string error;
        try { if (netplay::SettingsStore(directory).LoadLauncher(settings, error)) return settings; } catch (...) {}
        if (std::chrono::steady_clock::now() >= until || (cancelled && cancelled())) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
Report Prepare(const Preparation& input, const Account& account, const std::function<bool()>& cancelled) {
    wchar_t sidecar[MAX_PATH] = {};
    Metadata meta; meta.kind = input.crash ? "crash" : "problem";
    meta.appVersion = SF4E_APP_VERSION; meta.sourceRevision = SF4E_SOURCE_REVISION;
    meta.channel = ReportChannel(input.channel);
    if (!install::ResolveInstallFile(L"Sidecar.dll", sidecar, MAX_PATH)) throw std::runtime_error("Sidecar missing");
    meta.buildId = launcher::Sha256Hex(sidecar);
    if (meta.buildId.size() != 64) throw std::runtime_error("Sidecar hash");
    OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
    const auto rtl = reinterpret_cast<LONG (WINAPI*)(OSVERSIONINFOW*)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    meta.windowsVersion = rtl && rtl(&version) == 0 ? "Windows " + std::to_string(version.dwMajorVersion) + "." +
        std::to_string(version.dwMinorVersion) + " build " + std::to_string(version.dwBuildNumber) : "Windows (version unavailable)";
    const auto settings = netplay::SettingsStore::DefaultDirectory();
    if (settings.empty()) throw std::runtime_error("settings directory");
    return Collect(std::filesystem::path(settings) / L"logs", std::move(meta), input.crash.value_or(CrashContext{}), cancelled, account);
}
std::string Boundary() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("report boundary");
    wchar_t wide[40] = {}; StringFromGUID2(guid, wide, 40);
    std::string boundary = "EmberReport";
    for (const wchar_t c : wide) if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'))
        boundary += static_cast<char>(c);
    return boundary;
}
std::uint64_t RetrySeconds(const std::string& header) {
    std::uint64_t seconds = 600;
    if (!header.empty() && std::all_of(header.begin(), header.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        try { seconds = std::stoull(header); } catch (...) {}
    } else {
        SYSTEMTIME date{}; const std::wstring retry(header.begin(), header.end());
        if (WinHttpTimeToSystemTime(retry.c_str(), &date)) {
            FILETIME then{}, now{}; GetSystemTimeAsFileTime(&now);
            if (SystemTimeToFileTime(&date, &then)) {
                ULARGE_INTEGER a{}, b{}; a.LowPart = then.dwLowDateTime; a.HighPart = then.dwHighDateTime;
                b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
                seconds = a.QuadPart > b.QuadPart ? (a.QuadPart - b.QuadPart + 9999999) / 10000000 : 1;
            }
        }
    }
    return (std::max)(std::uint64_t(1), (std::min)(seconds, std::uint64_t(86400)));
}
bool SamePreview(const WorkflowState& state, const Submission& input) {
    return state.preview && input.preview == state.preview && state.id.empty() &&
        state.phase != Phase::Preparing && std::chrono::steady_clock::now() >= state.retryAt;
}
bool ValidId(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
// A short code from the intake's answer, so nothing else from it is kept.
std::string ReasonCode(const std::string& body) {
    const auto answer = nlohmann::json::parse(body, nullptr, false);
    const auto reason = answer.is_object() && answer.contains("reason") && answer["reason"].is_string() ? answer["reason"].get<std::string>() : std::string();
    if (reason.empty() || reason.size() > 40 || !std::all_of(reason.begin(), reason.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; })) return {};
    return reason;
}
// Sends `multipart` once and reads the answer into the state and the record.
void Upload(const Multipart& multipart, WorkflowState& state, Record& record, const std::function<bool()>& cancelled,
    const WorkflowDependencies& dependencies) {
    const auto uploaded = dependencies.upload ? dependencies.upload(multipart, cancelled) :
        HttpPostReport(ReportIntakeHost, multipart.contentType, multipart.body, cancelled);
    if (uploaded.request.ok) {
        const auto answer = nlohmann::json::parse(uploaded.body, nullptr, false);
        const auto id = answer.is_object() && answer.contains("id") && answer["id"].is_string() ? answer["id"].get<std::string>() : std::string();
        record.status = Status::Received;
        if (ValidId(id)) {
            state.id = record.id = id; state.phase = Phase::Sent; state.message = loc::Tf("reports.sent", id);
        } else { state.phase = Phase::Failed; state.message = loc::T("reports.invalid_response"); record.reason = "no_id"; }
    } else if (uploaded.cancelled || (cancelled && cancelled())) {
        state.phase = Phase::Cancelled; state.message = loc::T("reports.cancelled");
        record.status = Status::NotSent; record.reason = "cancelled";
    } else if (uploaded.request.statusCode == 429) {
        const auto seconds = RetrySeconds(uploaded.retryAfter);
        state.phase = Phase::Retry; state.retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        state.message = loc::Tf("reports.rate_limited", seconds);
        record.status = Status::Refused; record.reason = "rate_limit";
    } else {
        state.phase = Phase::Failed;
        state.message = loc::T(uploaded.request.statusCode == 503 ? "reports.unavailable" : "reports.failed");
        if (uploaded.request.statusCode > 0) {
            record.status = Status::Refused; record.reason = ReasonCode(uploaded.body);
            if (record.reason.empty()) record.reason = "http_" + std::to_string(uploaded.request.statusCode);
        } else { record.status = Status::NotSent; record.reason = uploaded.request.error == HttpErrorKind::Timeout ? "timeout" : "no_connection"; }
    }
}
}
bool ReportWorkflow::Begin(const Operation& operation, WorkflowState& state) {
    if (state.Busy()) return false;
    if (std::holds_alternative<Submission>(operation)) {
        if (!SamePreview(state, std::get<Submission>(operation))) return false;
        state.phase = Phase::Submitting; state.message = loc::T("reports.sending"); state.record.reset();
        state.automatic = state.authorized = state.offer = false;
        return true;
    }
    // A preview keeps the crash's offer, so closing it unsent offers it again.
    const auto preparation = state.preparation + 1;
    const bool offer = std::holds_alternative<Preparation>(operation) && state.offer;
    state = {}; state.preparation = preparation; state.phase = Phase::Preparing; state.offer = offer;
    state.automatic = std::holds_alternative<AutomaticCrash>(operation);
    state.message = state.automatic ? std::string() : std::string(loc::T("reports.preparing"));
    return true;
}
WorkflowState ReportWorkflow::Execute(const Operation& operation, WorkflowState state,
    const std::function<bool()>& cancelled, const WorkflowDependencies& dependencies) {
    const auto isCancelled = [&] { return cancelled && cancelled(); };
    const auto cancel = [&] { state.phase = Phase::Cancelled; state.message = loc::T("reports.cancelled"); };
    const auto history = dependencies.history ? dependencies.history : std::make_shared<ReportHistory>(SentReportsPath());
    const auto identity = [&] {
        return dependencies.identity ? dependencies.identity(cancelled) :
            CollectIdentity(netplay::SettingsStore::DefaultDirectory(), cancelled);
    };
    const auto collect = [&](const Preparation& preparation, const Account& account) {
        return dependencies.collect ? dependencies.collect(preparation, account, cancelled) : Prepare(preparation, account, cancelled);
    };
    state.record.reset();
    // A reserved send not yet completed: completed as not sent if anything
    // stops it, so the record never keeps a send that did not happen as pending.
    std::optional<Record> reserved;
    const auto finish = [&](Record record) {
        state.record = record;
        if (reserved) { (void)history->Complete(record); reserved.reset(); }
    };
    const auto notSent = [&](const char* reason) {
        if (!reserved) return;
        Record record = *reserved; record.status = Status::NotSent; record.reason = reason;
        finish(record);
    };
    try {
        if (isCancelled()) { cancel(); return state; }
        if (const auto* prepare = std::get_if<Preparation>(&operation)) {
            const auto who = identity();
            if (!who.ok) {
                if (isCancelled()) cancel(); else { state.phase = Phase::Failed; state.message = loc::T("reports.identity_failed"); }
                return state;
            }
            auto report = collect(*prepare, who.account);
            if (isCancelled()) cancel();
            else {
                state.preview = std::make_shared<const Report>(std::move(report)); state.phase = Phase::Preview;
                state.message = loc::T("reports.preview_detail");
            }
            return state;
        }
        if (const auto* automatic = std::get_if<AutomaticCrash>(&operation)) {
            state.automatic = true;
            const bool confirmed = automatic->consent == AutomaticCrash::Consent::Confirmed;
            // Allowed by the saved setting, or by Always send once it is saved.
            if (confirmed) {
                if (!(dependencies.turnReportsOn ? dependencies.turnReportsOn() : TurnReportsOn())) {
                    state.phase = Phase::Failed; state.message = loc::T("reports.always_failed"); state.offer = true;
                    return state;
                }
            } else {
                const auto on = dependencies.reportsOn ? dependencies.reportsOn() : ReportsOn();
                if (!on || !*on) { state.phase = Phase::Idle; state.offer = true; return state; }
            }
            // Counted before anything is read or sent; without a count, offered.
            Record attempt; attempt.kind = "crash";
            if (history->Reserve(attempt, true, Now()) != Reservation::Reserved) {
                state.phase = Phase::Idle; state.offer = true;
                if (confirmed) state.message = loc::T("reports.automatic_unavailable");
                return state;
            }
            reserved = attempt;
            state.authorized = true; state.phase = Phase::Submitting; state.message = loc::T("reports.sending");
            if (dependencies.progress) dependencies.progress(state);
            const auto who = identity();
            if (!who.ok) {
                notSent("not_prepared");
                if (isCancelled()) cancel(); else { state.phase = Phase::Failed; state.message = loc::T("reports.identity_failed"); }
                return state;
            }
            // Logs only: the small dump is never read for a report sent without a press.
            Preparation logsOnly{automatic->crash, automatic->channel};
            logsOnly.crash->smallDump.clear();
            auto report = collect(logsOnly, who.account);
            report.minidump.clear();
            if (isCancelled()) { notSent("cancelled"); cancel(); return state; }
            state.preview = std::make_shared<const Report>(std::move(report));
            Multipart multipart; std::string error;
            if (!BuildMultipart(*state.preview, Submission{std::string(), false, state.preview}, Boundary(), multipart, error)) {
                notSent("invalid"); state.phase = Phase::Failed; state.message = loc::T("reports.invalid"); return state;
            }
            Record record = attempt;
            Upload(multipart, state, record, cancelled, dependencies);
            finish(record);
            return state;
        }
        const auto& submission = std::get<Submission>(operation);
        Multipart multipart; std::string error;
        // Revalidate at the actual upload boundary, even if Begin accepted it.
        if (!SamePreview(state, submission) || !BuildMultipart(*state.preview, submission, Boundary(), multipart, error)) {
            state.phase = Phase::Failed; state.message = loc::T("reports.invalid"); return state;
        }
        if (isCancelled()) { cancel(); return state; }
        // Counted before the upload starts. A record that cannot be read
        // already stops every automatic send, so this one goes uncounted.
        Record attempt; attempt.kind = state.preview->meta.kind;
        attempt.dump = submission.includeDump && !state.preview->minidump.empty();
        const auto reservation = history->Reserve(attempt, false, Now());
        if (reservation == Reservation::Failed || reservation == Reservation::LimitReached) {
            state.phase = Phase::Failed; state.message = loc::T("reports.history_failed"); return state;
        }
        if (reservation == Reservation::Reserved) reserved = attempt;
        else attempt.time = Now();
        Record record = attempt;
        Upload(multipart, state, record, cancelled, dependencies);
        finish(record);
    } catch (...) {
        notSent(isCancelled() ? "cancelled" : "failed");
        if (isCancelled()) cancel();
        else { state.phase = Phase::Failed; state.message = loc::T("services.operation_failed"); }
    }
    return state;
}
std::optional<bool> ReportsOn() {
    const auto saved = LoadSettings(netplay::SettingsStore::DefaultDirectory(), {}, std::chrono::milliseconds(3000));
    if (!saved) return std::nullopt;
    const char* key = SendReportsKey();
    if (!saved->contains(key)) return false;
    if (!(*saved)[key].is_boolean()) return std::nullopt;
    return (*saved)[key].get<bool>();
}
bool TurnReportsOn() {
    const auto directory = netplay::SettingsStore::DefaultDirectory();
    if (directory.empty()) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    for (;;) {
        std::string error;
        try { if (netplay::SettingsStore(directory).SaveLauncher({{SendReportsKey(), true}}, error)) return true; } catch (...) {}
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
Identity CollectIdentity(const std::wstring& settingsDirectory, const std::function<bool()>& cancelled, std::chrono::milliseconds wait) {
    Identity identity;
    const auto saved = LoadSettings(settingsDirectory, cancelled, wait);
    if (!saved) return identity;
    std::string player;
    if (saved->contains("displayName")) {
        if (!(*saved)["displayName"].is_string()) return identity;
        player = (*saved)["displayName"].get<std::string>();
    }
    auto& names = identity.account.names;
    PWSTR profile = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &profile)) || !profile) {
        if (profile) CoTaskMemFree(profile);
        return identity;
    }
    const std::wstring path = profile;
    CoTaskMemFree(profile);
    names.push_back(platform::WideToUtf8(std::filesystem::path(path).filename().wstring()));
    // The 8.3 form of the profile folder; without one Windows gives the long path back.
    const DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (!needed) return identity;
    std::wstring shortPath(needed, L'\0');
    const DWORD length = GetShortPathNameW(path.c_str(), &shortPath[0], needed);
    if (!length || length >= needed) return identity;
    shortPath.resize(length);
    names.push_back(platform::WideToUtf8(std::filesystem::path(shortPath).filename().wstring()));
    wchar_t user[257] = {}; DWORD userLength = 257;
    if (!GetUserNameW(user, &userLength)) return identity;
    names.push_back(platform::WideToUtf8(user));
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {}; DWORD computerLength = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(computer, &computerLength)) return identity;
    names.push_back(platform::WideToUtf8(computer));
    // "Player" is the name every new profile starts with, so it names no one.
    if (!player.empty() && player != "Player") names.push_back(player);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    identity.ok = true;
    return identity;
}
} }
