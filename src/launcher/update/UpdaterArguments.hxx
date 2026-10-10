#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "PackageInstaller.hxx"

// The command line the launcher starts Updater.exe with, and its -Stages
// field, both ways: what the launcher writes is what the Updater reads, the
// translated text as it was. Nothing here starts a process.
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

// -Stages: one name per installer step (InstallerSteps), joined by '|'. False,
// leaving field empty, when a name is missing or empty or holds '|' or a
// control character, which the field cannot carry: the Updater's window then
// shows its bar without step names.
inline bool JoinStageNames(const std::vector<std::wstring>& names, std::wstring& field) {
    field.clear();
    if (names.size() != InstallerStepCount) return false;
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
// The Updater's side: the names in InstallerSteps' order, or none when the
// field does not hold exactly one non-empty name per step.
inline std::vector<std::wstring> SplitStageNames(const wchar_t* field) {
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
    if (names.size() != InstallerStepCount) names.clear();
    return names;
}

// The longest argument line passed, in characters. The Updater is started
// with ShellExecuteEx, whose own limit on lpParameters is not documented, so
// this stays within the 4,096 characters the handoff has always passed, far
// below the 32,767 of CreateProcess. Two full paths, the status and five
// step names take well under half of it.
constexpr std::size_t MostUpdaterArguments = 4096 - 1;

struct UpdaterArguments {
    std::wstring installDir, stagingDir;
    std::uint32_t waitPid = 0;
    bool recoverOnly = false;
    // The window's text and its steps' names, in the player's language; an
    // Updater.exe from before either ignores it.
    std::wstring status;
    std::vector<std::wstring> stages;
};
// The arguments after Updater.exe's path. -Stages is left out when its names
// cannot be carried (JoinStageNames). False, leaving out empty, when the line
// would be longer than Windows starts a process with.
inline bool BuildUpdaterArguments(const UpdaterArguments& in, std::wstring& out) {
    out.clear();
    std::wstring line = L"-InstallDir " + QuoteArgument(in.installDir);
    if (in.recoverOnly) line += L" -RecoverOnly";
    else line += L" -StagingDir " + QuoteArgument(in.stagingDir);
    line += L" -WaitPid " + std::to_wstring(in.waitPid);
    if (!in.status.empty()) line += L" -Status " + QuoteArgument(in.status);
    std::wstring stages;
    if (!in.stages.empty() && JoinStageNames(in.stages, stages)) line += L" -Stages " + QuoteArgument(stages);
    if (line.size() > MostUpdaterArguments) return false;
    out = line;
    return true;
}

} }
