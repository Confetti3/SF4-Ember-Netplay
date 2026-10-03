#include "../launcher/update/ArchiveTool.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

static bool Run(const std::wstring& exe, const std::wstring& arguments) {
    auto command = L"\"" + exe + L"\" " + arguments;
    std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 15000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}
int main() {
    namespace fs = std::filesystem;
    const auto tool = sf4e::launcher::detail::ArchiveToolPath();
    BOOL wow64 = FALSE; CHECK(IsWow64Process(GetCurrentProcess(), &wow64));
    CHECK(!tool.empty());
    CHECK((tool.find(L"\\Sysnative\\") != std::wstring::npos) == (wow64 != FALSE));
    CHECK(GetFileAttributesW(tool.c_str()) != INVALID_FILE_ATTRIBUTES);
    const auto root = fs::temp_directory_path() / (L"ember archive test " + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root); fs::create_directories(root / L"input"); fs::create_directories(root / L"output");
    const std::string payload = "verified archive extraction\n";
    { std::ofstream out(root / L"input" / L"payload.txt", std::ios::binary); out << payload; }
    const auto quote = [](const fs::path& p) { return L"\"" + p.wstring() + L"\""; };
    // Exercise ZIP extraction from the same x86 process as the updater. Every
    // path contains spaces, and no PATH lookup can supply a different tar.
    CHECK(Run(tool, L"-a -cf " + quote(root / L"package.zip") + L" -C " + quote(root / L"input") + L" payload.txt"));
    CHECK(Run(tool, L"-xf " + quote(root / L"package.zip") + L" -C " + quote(root / L"output")));
    std::ifstream in(root / L"output" / L"payload.txt", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == payload);
    in.close(); fs::remove_all(root);
    std::cout << "Native Windows ZIP extraction passed (WOW64=" << wow64 << ")\n";
}
