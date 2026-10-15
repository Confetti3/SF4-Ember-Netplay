#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#include "PackageInstaller.hxx"

// The command line the launcher starts Updater.exe with, and its -Stages and
// -Notices fields, both ways: what the launcher writes is what the Updater
// reads, the translated text as it was. Nothing here starts a process.
namespace sf4e { namespace launcher {

// value as one argument that CommandLineToArgvW, and the C runtime's argv,
// read back unchanged: in quotes, with each quote escaped and the
// backslashes before a quote, or before the closing quote, doubled.
inline std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') quoted.append(backslashes * 2 + 1, L'\\');
        else quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted += c;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

// The Updater window's texts besides the steps' names, in the order -Notices
// carries them: each with the locale key the launcher passes in the player's
// language and its English text, the key's en.po text, for a launcher from
// before -Notices. {0} is the file waited for, or the time taken so far.
enum class UpdaterNotice { WaitingForLauncher, WaitingForFile, Restoring, Elapsed, Reopens, Recovering };
struct UpdaterNoticeText { UpdaterNotice notice; const char* key; const wchar_t* english; };
inline constexpr UpdaterNoticeText UpdaterNotices[] = {
    { UpdaterNotice::WaitingForLauncher, "update.notice.waiting_launcher", L"Waiting for SF4 Ember Netplay to close..." },
    { UpdaterNotice::WaitingForFile, "update.notice.waiting_file", L"Waiting for another program to release {0}..." },
    { UpdaterNotice::Restoring, "update.notice.restoring", L"Restoring the previous files..." },
    { UpdaterNotice::Elapsed, "update.notice.elapsed", L"{0} elapsed" },
    { UpdaterNotice::Reopens, "update.notice.reopens", L"SF4 Ember Netplay opens again when it is done." },
    { UpdaterNotice::Recovering, "update.notice.recovering", L"Finishing an interrupted update..." } };
inline constexpr std::size_t UpdaterNoticeCount = sizeof(UpdaterNotices) / sizeof(UpdaterNotices[0]);
inline constexpr bool UpdaterNoticesInOrder() {
    for (std::size_t i = 0; i < UpdaterNoticeCount; ++i) if (static_cast<std::size_t>(UpdaterNotices[i].notice) != i) return false;
    return true;
}
static_assert(UpdaterNoticesInOrder(), "updater notices out of their enum's order");
// The notices as the launcher passed them, or their English texts when that
// is not one per notice.
inline std::vector<std::wstring> NoticeTexts(const std::vector<std::wstring>& passed) {
    if (passed.size() == UpdaterNoticeCount) return passed;
    std::vector<std::wstring> english;
    for (const auto& notice : UpdaterNotices) english.push_back(notice.english);
    return english;
}

// A field of `count` names, joined by '|': -Stages, one per installer step
// (InstallerSteps), and -Notices, one per UpdaterNotices entry. False, leaving
// field empty, when a name is missing or empty or holds '|' or a control
// character, which the field cannot carry: the Updater's window then shows
// that field's English texts.
inline bool JoinNames(const std::vector<std::wstring>& names, std::size_t count, std::wstring& field) {
    field.clear();
    if (names.size() != count) return false;
    std::wstring joined;
    for (const auto& name : names) {
        if (name.empty()) return false;
        for (const wchar_t c : name) if (c == L'|' || c < 0x20 || c == 0x7F) return false;
        if (!joined.empty()) joined += L'|';
        joined += name;
    }
    field = joined;
    return true;
}
// The Updater's side: the names in order, or none when the field does not
// hold exactly `count` non-empty names.
inline std::vector<std::wstring> SplitNames(const wchar_t* field, std::size_t count) {
    std::vector<std::wstring> names;
    if (!field || !field[0]) return names;
    std::wstring rest(field);
    for (std::size_t start = 0;;) {
        const std::size_t bar = rest.find(L'|', start);
        names.push_back(rest.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (names.back().empty()) return {};
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    if (names.size() != count) names.clear();
    return names;
}

// The longest argument line passed, in characters. The Updater is started
// with ShellExecuteEx, whose own limit on lpParameters is not documented, so
// this stays within the 4,096 characters the handoff has always passed, far
// below the 32,767 of CreateProcess. Two full paths, the status, five step
// names and six notices take well under half of it.
constexpr std::size_t MostUpdaterArguments = 4096 - 1;

struct UpdaterArguments {
    std::wstring installDir, stagingDir;
    std::uint32_t waitPid = 0;
    // Setup's uninstaller passes -Uninstall; the launcher never does.
    bool recoverOnly = false, uninstall = false;
    // What an install's headline says, its steps' names and its notices, in
    // the player's language; an Updater.exe from before any ignores it.
    std::wstring status;
    std::vector<std::wstring> stages, notices;
};
// What an install's headline says when the launcher passed no -Status, as one
// from before it does: services.installing's en.po text.
inline constexpr wchar_t DefaultUpdaterStatus[] = L"Installing the update...";
// Whether the Updater starts the launcher again when it is done: always after
// an install, and after a recovery the launcher asked for at startup (with
// -WaitPid), not one started by hand.
inline bool UpdaterReopensLauncher(const UpdaterArguments& in) { return !in.recoverOnly || in.waitPid != 0; }
// The Updater window's headline: what this run does (an install's -Status, or
// finishing an interrupted update), then, on its own line, that Ember opens
// again, whenever it does. The second line is the Updater's own notice, so a
// launcher whose -Status lacks it, as every one before -Notices, still has it
// said.
inline std::wstring UpdaterHeadline(const UpdaterArguments& in, const std::vector<std::wstring>& notices) {
    std::wstring headline = in.recoverOnly ? notices[static_cast<std::size_t>(UpdaterNotice::Recovering)] :
        in.status.empty() ? std::wstring(DefaultUpdaterStatus) : in.status;
    if (UpdaterReopensLauncher(in)) headline += L"\n" + notices[static_cast<std::size_t>(UpdaterNotice::Reopens)];
    return headline;
}
// The arguments after Updater.exe's path. -Stages or -Notices is left out when
// its names cannot be carried (JoinNames). False, leaving out empty, when the
// line would be longer than Windows starts a process with.
inline bool BuildUpdaterArguments(const UpdaterArguments& in, std::wstring& out) {
    out.clear();
    std::wstring line = L"-InstallDir " + QuoteArgument(in.installDir);
    if (in.uninstall) line += L" -Uninstall";
    else if (in.recoverOnly) line += L" -RecoverOnly";
    else line += L" -StagingDir " + QuoteArgument(in.stagingDir);
    line += L" -WaitPid " + std::to_wstring(in.waitPid);
    if (!in.status.empty()) line += L" -Status " + QuoteArgument(in.status);
    std::wstring field;
    if (!in.stages.empty() && JoinNames(in.stages, InstallerStepCount, field)) line += L" -Stages " + QuoteArgument(field);
    if (!in.notices.empty() && JoinNames(in.notices, UpdaterNoticeCount, field)) line += L" -Notices " + QuoteArgument(field);
    if (line.size() > MostUpdaterArguments) return false;
    out = line;
    return true;
}
// The Updater's side: the arguments from its argv, names in any case. False
// without the folders its work needs: -InstallDir always, and -StagingDir
// unless it recovers or uninstalls. An empty -Status, and a -Stages or
// -Notices that SplitNames cannot read, leave theirs empty.
inline bool ParseUpdaterArguments(int argc, const wchar_t* const* argv, UpdaterArguments& out) {
    out = UpdaterArguments{};
    for (int i = 1; i < argc; ++i) {
        const auto is = [&](const wchar_t* name) { return _wcsicmp(argv[i], name) == 0; };
        const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (is(L"-RecoverOnly")) out.recoverOnly = true;
        else if (is(L"-Uninstall")) out.uninstall = true;
        else if (!value) continue;
        else if (is(L"-InstallDir")) out.installDir = argv[++i];
        else if (is(L"-StagingDir")) out.stagingDir = argv[++i];
        else if (is(L"-WaitPid")) out.waitPid = static_cast<std::uint32_t>(wcstoul(argv[++i], nullptr, 10));
        else if (is(L"-Status")) out.status = argv[++i];
        else if (is(L"-Stages")) out.stages = SplitNames(argv[++i], InstallerStepCount);
        else if (is(L"-Notices")) out.notices = SplitNames(argv[++i], UpdaterNoticeCount);
    }
    return !out.installDir.empty() && (out.recoverOnly || out.uninstall || !out.stagingDir.empty());
}

} }
