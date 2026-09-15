#include "../launcher/update/PackageInstaller.hxx"
#include "../common/PackageInventory.hxx"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#define CHECK(c) do { if (!(c)) { std::cerr << "Failure at " << __LINE__ << ": " << error << '\n'; std::exit(1); } } while (false)
namespace fs = std::filesystem;
void Write(const fs::path& path, const char* text) { fs::create_directories(path.parent_path()); std::ofstream(path) << text; }
std::string Read(const fs::path& path) { std::ifstream input(path); return {std::istreambuf_iterator<char>(input), {}}; }
int wmain(int argc, wchar_t** argv) {
    std::string error;
    if (argc == 3 && std::wstring(argv[1]) == L"--recover") {
        if(sf4e::launcher::RecoverPackage(argv[2],error)) return 0;
        std::cerr<<error<<'\n'; return 2;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--crash-child") {
        const fs::path root(argv[2]);
        return sf4e::launcher::InstallPackage(root/L"staging",root/L"install",error) ? 0 : 2;
    }
    if (argc == 2) {
        const fs::path package(argv[1]);
        for (const auto* path : sf4e::package::Required) CHECK(fs::is_regular_file(package/path));
        for (const auto& entry : fs::recursive_directory_iterator(package)) if (entry.is_regular_file()) {
            const auto path = entry.path().lexically_relative(package);
            if (!sf4e::package::IsAllowed(path.c_str())) { std::wcerr << L"Unexpected package file: " << path.c_str() << L'\n'; return 1; }
        }
        std::cout << "Actual package accepted by the native updater inventory\n";
        return 0;
    }
    const auto root = fs::temp_directory_path() / (L"ember-upgrade-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto staging = root/L"staging", install = root/L"install";
    fs::create_directories(staging); fs::create_directories(install);
    for (const auto* path : sf4e::package::Required) Write(staging/path,"new");
    Write(install/L"Launcher.exe","old"); Write(install/L"Qt6Core.dll","legacy");
    Write(install/L"dxwrapper.dll","previous-display-wrapper");
    Write(install/L"Safe display.cmd","previous-display-recovery");
    Write(install/L"d3d9.dll","user-owned-proxy");
    Write(install/L"my-replay.bin","user"); Write(install/L"settings.json.pre-v1.bak","backup");
    Write(install/L"Launcher.exe.ember-new","user-temporary-name");
    Write(install/L".ember-update-transaction-v1.json.new","user-journal-name");
    CHECK(sf4e::launcher::RecoverPackage(install,error,true));
    CHECK(!fs::exists(install/L".ember-update.lock"));
    Write(staging/L"unexpected.exe","reject");
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe") == "old"); fs::remove(staging/L"unexpected.exe");
    CHECK(sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe") == "new" && !fs::exists(install/L"Qt6Core.dll"));
    CHECK(!fs::exists(install/L"dxwrapper.dll") && !fs::exists(install/L"Safe display.cmd"));
    CHECK(Read(install/L"d3d9.dll") == "user-owned-proxy");
    CHECK(Read(install/L"my-replay.bin") == "user" && Read(install/L"settings.json.pre-v1.bak") == "backup");
    CHECK(Read(install/L"Launcher.exe.ember-new")=="user-temporary-name");
    CHECK(Read(install/L".ember-update-transaction-v1.json.new")=="user-journal-name");
    bool backed = false, displayBacked = false;
    for (const auto& item : fs::recursive_directory_iterator(install/L".ember-update-backups")) {
        if (item.path().filename() == L"Qt6Core.dll") backed = Read(item.path()) == "legacy";
        if (item.path().filename() == L"dxwrapper.dll") displayBacked = Read(item.path()) == "previous-display-wrapper";
    }
    CHECK(backed && displayBacked);
    // A late obsolete-file failure must roll back earlier replacements.
    Write(staging/L"Launcher.exe","third"); fs::create_directory(install/L"Qt6Core.dll");
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe") == "new"); fs::remove(install/L"Qt6Core.dll");
    fs::remove(staging/L"sf4-net.exe");
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error)); CHECK(Read(install/L"Launcher.exe") == "new");
    CHECK(!sf4e::package::IsAllowed(L"../outside.exe"));
    CHECK(!sf4e::package::IsAllowed(L"plugins/platforms/arbitrary.dll"));

    // A process death after a replacement leaves a durable transaction. The
    // next recovery restores both product files and an occupied newly-added
    // destination from exact backup bytes.
    const auto crashRoot=root/L"crash", crashStaging=crashRoot/L"staging", crashInstall=crashRoot/L"install";
    fs::create_directories(crashStaging);fs::create_directories(crashInstall);
    for(const auto* path:sf4e::package::Required) Write(crashStaging/path,"target");
    Write(crashInstall/L"Launcher.exe","prior-launcher");
    Write(crashInstall/L"sf4-net.exe","user-collision");
    std::wstring command=L"\""+fs::absolute(argv[0]).wstring()+L"\" --crash-child \""+crashRoot.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    SetEnvironmentVariableW(L"SF4E_UPDATE_TEST_TERMINATE_AFTER",L"2");
    CHECK(CreateProcessW(nullptr,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child));
    SetEnvironmentVariableW(L"SF4E_UPDATE_TEST_TERMINATE_AFTER",nullptr);
    WaitForSingleObject(child.hProcess,30000);DWORD exitCode=0;GetExitCodeProcess(child.hProcess,&exitCode);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);CHECK(exitCode==86);
    CHECK(fs::is_regular_file(crashInstall/L".ember-update-transaction-v1.json"));
    const auto journalPath=crashInstall/L".ember-update-transaction-v1.json";
    auto journal=nlohmann::json::parse(Read(journalPath));
    const auto originalJournal=journal;
    const auto beforeRecovery=Read(crashInstall/L"Launcher.exe");
    // Invalid late operations must fail before any earlier operation restores.
    journal["operations"].push_back({{"path","../outside.txt"},{"existed",false},{"priorSha256",""}});
    Write(journalPath,journal.dump().c_str());
    CHECK(!sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")==beforeRecovery);
    journal=originalJournal;
    for(auto& op:journal["operations"]) if(op["path"]=="sf4-net.exe") op["priorSha256"]=std::string(64,'0');
    Write(journalPath,journal.dump().c_str());
    CHECK(!sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")==beforeRecovery);
    // PowerShell emits uppercase SHA-256 digests. Both readers accept them.
    journal=originalJournal;
    for(auto& op:journal["operations"]) {
        auto digest=op["priorSha256"].get<std::string>();
        for(auto& c:digest) c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        op["priorSha256"]=digest;
    }
    Write(journalPath,journal.dump().c_str());
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")=="prior-launcher");
    CHECK(Read(crashInstall/L"sf4-net.exe")=="user-collision");
    CHECK(!fs::exists(crashInstall/L".ember-update-transaction-v1.json"));
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error)); // idempotent restart
    // Only this uniquely created temporary fixture is removed.
    fs::remove_all(root);
    std::cout << "Inventory, upgrade preservation, backup and rollback checks passed\n";
}
