#include "../launcher/update/PackageInstaller.hxx"
#include "../common/PackageInventory.hxx"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::cerr << "Failure at " << __LINE__ << ": " << error << '\n'; std::exit(1); } } while (false)
namespace fs = std::filesystem;
void Write(const fs::path& path, const char* text) { fs::create_directories(path.parent_path()); std::ofstream(path) << text; }
std::string Read(const fs::path& path) { std::ifstream input(path); return {std::istreambuf_iterator<char>(input), {}}; }
int wmain(int argc, wchar_t** argv) {
    std::string error;
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
    Write(staging/L"unexpected.exe","reject");
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe") == "old"); fs::remove(staging/L"unexpected.exe");
    CHECK(sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe") == "new" && !fs::exists(install/L"Qt6Core.dll"));
    CHECK(!fs::exists(install/L"dxwrapper.dll") && !fs::exists(install/L"Safe display.cmd"));
    CHECK(Read(install/L"d3d9.dll") == "user-owned-proxy");
    CHECK(Read(install/L"my-replay.bin") == "user" && Read(install/L"settings.json.pre-v1.bak") == "backup");
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
    // Only this uniquely created temporary fixture is removed.
    fs::remove_all(root);
    std::cout << "Inventory, upgrade preservation, backup and rollback checks passed\n";
}
