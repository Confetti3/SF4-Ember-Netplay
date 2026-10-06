#include "../launcher/update/PackageInstaller.hxx"
#include "../common/PackageInventory.hxx"
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "temp_root.hxx"
#define CHECK(c) do { if (!(c)) { std::cerr << "Failure at " << __LINE__ << ": " << error << '\n'; std::exit(1); } } while (false)
namespace fs = std::filesystem;
void Write(const fs::path& path, const char* text) { fs::create_directories(path.parent_path()); std::ofstream(path) << text; }
std::string Read(const fs::path& path) { std::ifstream input(path); return {std::istreambuf_iterator<char>(input), {}}; }
std::string Sha256(const std::string& bytes) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; unsigned char digest[32]{};
    BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0);
    BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);
    static const char hex[]="0123456789abcdef"; std::string text;
    for(const auto byte:digest){text+=hex[byte>>4];text+=hex[byte&15];}
    return text;
}
// MANIFEST.txt for every file in the folder, as packaging writes it.
void Manifest(const fs::path& package) {
    std::string lines;
    for (const auto& entry : fs::recursive_directory_iterator(package)) {
        const auto rel = entry.path().lexically_relative(package);
        if (entry.is_regular_file() && rel != L"MANIFEST.txt") lines += sf4e::launcher::Sha256Hex(entry.path()) + "  " + rel.u8string() + "\n";
    }
    Write(package/L"MANIFEST.txt", lines.c_str());
}
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
    const auto root = MakeTempRoot(L"ember-upgrade-test-");
    const auto staging = root/L"staging", install = root/L"install";
    fs::create_directories(staging); fs::create_directories(install);
    for (const auto* path : sf4e::package::Required) Write(staging/path,"new");
    Manifest(staging);
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
    // A package answers to its own manifest: a changed file, a file it does
    // not name and a named file that is missing are each refused untouched.
    Write(staging/L"Launcher.exe","tampered");
    CHECK(!sf4e::launcher::ValidatePackageFolder(staging,error) && !sf4e::launcher::InstallPackage(staging,install,error) && Read(install/L"Launcher.exe")=="old");
    Write(staging/L"Launcher.exe","new");
    Write(staging/L"docs\\TRAINING_LAB.md","unlisted");
    CHECK(!sf4e::launcher::ValidatePackageFolder(staging,error)); fs::remove(staging/L"docs\\TRAINING_LAB.md"); fs::remove(staging/L"docs");
    CHECK(sf4e::launcher::ValidatePackageFolder(staging,error));
    // This version ships a doc and a selection asset the older one below lacks.
    Write(staging/L"docs\\TRAINING_LAB.md","new"); Write(staging/L"assets\\selection\\sources.json","new"); Manifest(staging);
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
    // H-013: another update replaces the backup set instead of adding one.
    CHECK(sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(std::distance(fs::directory_iterator(install/L".ember-update-backups"), fs::directory_iterator()) == 1);
    const auto keptBackup = fs::directory_iterator(install/L".ember-update-backups")->path();
    // A late obsolete-file failure must roll back earlier replacements.
    Write(staging/L"Launcher.exe","third"); Manifest(staging); fs::create_directory(install/L"Qt6Core.dll");
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe") == "new"); fs::remove(install/L"Qt6Core.dll");
    fs::remove(staging/L"sf4-net.exe");
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error)); CHECK(Read(install/L"Launcher.exe") == "new");
    // Failed or invalid updates never discard the last good backup set.
    CHECK(fs::exists(keptBackup));
    Write(staging/L"sf4-net.exe","new"); Write(staging/L"Launcher.exe","new"); Manifest(staging);

    // One transition, whichever way the versions go: the folder's product
    // files become exactly the package's. Here an older package: it lacks the
    // doc and the selection asset the installed manifest names, so those go
    // (with a rollback copy, the player's edit included), and it ships a file
    // this build lists as obsolete, which is accepted. Art the player keeps at
    // an accepted name that no package shipped is not an update's to take.
    Write(install/L"docs\\TRAINING_LAB.md","player-edited");
    Write(install/L"assets\\selection\\horror-sources.json","player-art");
    Write(install/L"my-replay.bin","user");
    const auto older=root/L"older"; fs::create_directories(older);
    for (const auto* path : sf4e::package::Required) Write(older/path,"older");
    Write(older/L"dxwrapper.dll","older-display-wrapper"); Manifest(older);
    CHECK(sf4e::launcher::ValidatePackageFolder(older,error));
    CHECK(sf4e::launcher::InstallPackage(older,install,error));
    CHECK(Read(install/L"Launcher.exe")=="older" && Read(install/L"dxwrapper.dll")=="older-display-wrapper");
    CHECK(!fs::exists(install/L"docs\\TRAINING_LAB.md") && !fs::exists(install/L"assets\\selection\\sources.json") && !fs::exists(install/L"docs"));
    CHECK(Read(install/L"my-replay.bin")=="user" && Read(install/L"d3d9.dll")=="user-owned-proxy");
    CHECK(Read(install/L"assets\\selection\\horror-sources.json")=="player-art");
    bool editKept=false;
    for (const auto& item : fs::recursive_directory_iterator(install/L".ember-update-backups"))
        if (item.path().filename()==L"TRAINING_LAB.md") editKept = Read(item.path())=="player-edited";
    CHECK(editKept);
    // And forward again: the newer package removes the obsolete file; the
    // player's art survives its second update.
    CHECK(sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe")=="new" && !fs::exists(install/L"dxwrapper.dll") && Read(install/L"docs\\TRAINING_LAB.md")=="new");
    CHECK(Read(install/L"assets\\selection\\horror-sources.json")=="player-art");
    // A product file held open cannot be removed: nothing changes, the
    // journal is gone, and the next transition succeeds.
    Write(install/L"docs\\TRAINING_LAB.md","held");
    HANDLE heldFile=CreateFileW((install/L"docs\\TRAINING_LAB.md").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(heldFile!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::InstallPackage(older,install,error) && error.find("restored")!=std::string::npos);
    CloseHandle(heldFile);
    CHECK(Read(install/L"Launcher.exe")=="new" && Read(install/L"docs\\TRAINING_LAB.md")=="held" && !fs::exists(install/L".ember-update-transaction-v1.json"));
    CHECK(sf4e::launcher::InstallPackage(older,install,error) && Read(install/L"Launcher.exe")=="older" && !fs::exists(install/L"docs"));
    CHECK(sf4e::launcher::InstallPackage(staging,install,error) && Read(install/L"Launcher.exe")=="new");
    CHECK(!sf4e::package::IsAllowed(L"../outside.exe"));
    CHECK(!sf4e::package::IsAllowed(L"plugins/platforms/arbitrary.dll"));

    // A process death after a replacement leaves a durable transaction. The
    // next recovery restores both product files and an occupied newly-added
    // destination from exact backup bytes.
    const auto crashRoot=root/L"crash", crashStaging=crashRoot/L"staging", crashInstall=crashRoot/L"install";
    fs::create_directories(crashStaging);fs::create_directories(crashInstall);
    for(const auto* path:sf4e::package::Required) Write(crashStaging/path,"target");
    Manifest(crashStaging);
    Write(crashInstall/L"Launcher.exe","prior-launcher");
    Write(crashInstall/L"sf4-net.exe","user-collision");
    // Owned by the installed manifest and absent from the package: a removal.
    Write(crashInstall/L"docs\\TRAINING_LAB.md","removed-first");
    const std::string crashManifest=Sha256("removed-first")+"  docs\\TRAINING_LAB.md\n";
    Write(crashInstall/L"MANIFEST.txt",crashManifest.c_str());
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
    // The removal that ran before the replacements is undone too.
    CHECK(Read(crashInstall/L"docs\\TRAINING_LAB.md")=="removed-first");
    CHECK(!fs::exists(crashInstall/L".ember-update-transaction-v1.json"));
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error)); // idempotent restart
    // A death right after the commit leaves a committed journal: the next
    // recovery only clears it, the folder is the target.
    std::wstring commitCommand=L"\""+fs::absolute(argv[0]).wstring()+L"\" --crash-child \""+crashRoot.wstring()+L"\"";
    STARTUPINFOW commitStartup{};commitStartup.cb=sizeof(commitStartup);PROCESS_INFORMATION commitChild{};
    SetEnvironmentVariableW(L"SF4E_UPDATE_TEST_TERMINATE_AFTER",L"commit");
    CHECK(CreateProcessW(nullptr,&commitCommand[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&commitStartup,&commitChild));
    SetEnvironmentVariableW(L"SF4E_UPDATE_TEST_TERMINATE_AFTER",nullptr);
    WaitForSingleObject(commitChild.hProcess,30000);DWORD commitExit=0;GetExitCodeProcess(commitChild.hProcess,&commitExit);
    CloseHandle(commitChild.hThread);CloseHandle(commitChild.hProcess);CHECK(commitExit==86);
    CHECK(nlohmann::json::parse(Read(journalPath))["state"]=="committed");
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")=="target" && !fs::exists(crashInstall/L"docs") && !fs::exists(journalPath));
    // Back to the interrupted state for the checks below.
    Write(crashInstall/L"Launcher.exe","prior-launcher"); Write(crashInstall/L"sf4-net.exe","user-collision"); Write(crashInstall/L"docs\\TRAINING_LAB.md","removed-first");
    Write(crashInstall/L"MANIFEST.txt",crashManifest.c_str());
    for(const auto& op:originalJournal["operations"]) if(!op["existed"].get<bool>()) fs::remove(crashInstall/fs::u8path(op["path"].get<std::string>()));

    // Recovery never locks a player out or downgrades a folder it does not own.
    const auto failedJournal=crashInstall/L".ember-update-transaction-v1.json.failed";
    // The damaged journals above were each reported, then set aside.
    CHECK(fs::exists(failedJournal)); fs::remove(failedJournal);
    // An update that in fact finished is left in place.
    for(const auto& op:originalJournal["operations"]) {
        const auto path=crashInstall/fs::u8path(op["path"].get<std::string>());
        if(originalJournal["target"].contains(op["path"].get<std::string>())) fs::copy_file(crashStaging/fs::u8path(op["path"].get<std::string>()),path,fs::copy_options::overwrite_existing); else fs::remove(path);
    }
    Write(journalPath,originalJournal.dump().c_str());
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")=="target" && !fs::exists(journalPath) && !fs::exists(failedJournal));
    // A package extracted over the folder since is not rolled back.
    Write(crashInstall/L"Launcher.exe","extracted-later");
    Write(journalPath,originalJournal.dump().c_str());
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")=="extracted-later" && !fs::exists(journalPath) && fs::exists(failedJournal));
    // A file that went missing (antivirus, say) is not a replaced folder: the
    // half-applied update is restored.
    for(const auto& op:originalJournal["operations"]) {
        const auto path=crashInstall/fs::u8path(op["path"].get<std::string>());
        if(originalJournal["target"].contains(op["path"].get<std::string>())) fs::copy_file(crashStaging/fs::u8path(op["path"].get<std::string>()),path,fs::copy_options::overwrite_existing); else fs::remove(path);
    }
    fs::remove(crashInstall/L"sf4-net.exe");
    Write(journalPath,originalJournal.dump().c_str());
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")=="prior-launcher" && Read(crashInstall/L"sf4-net.exe")=="user-collision" && !fs::exists(journalPath));
    // A journal that cannot be cleared is a failure, never a reported success,
    // or the Launcher and Updater would restart each other forever.
    Write(crashInstall/L"Launcher.exe","extracted-later");
    Write(journalPath,originalJournal.dump().c_str());
    HANDLE held=CreateFileW(journalPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(held!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::RecoverPackage(crashInstall,error) && error.find("Cannot clear")!=std::string::npos);
    CloseHandle(held);
    CHECK(fs::exists(journalPath) && Read(crashInstall/L"Launcher.exe")=="extracted-later");
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(!fs::exists(journalPath) && Read(crashInstall/L"Launcher.exe")=="extracted-later");
    // A committed journal whose files changed afterwards is only cleared.
    auto committedJournal=originalJournal; committedJournal["state"]="committed";
    Write(journalPath,committedJournal.dump().c_str());
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(Read(crashInstall/L"Launcher.exe")=="extracted-later" && !fs::exists(journalPath));
    // A journal that can never be recovered is reported once, then set aside.
    fs::remove(failedJournal);
    Write(journalPath,"{ damaged");
    CHECK(!sf4e::launcher::RecoverPackage(crashInstall,error));
    CHECK(!fs::exists(journalPath) && fs::exists(failedJournal));
    CHECK(sf4e::launcher::RecoverPackage(crashInstall,error));
    // Uninstall removes what the inventory names or allows, an update's
    // additions and the updater's state; the player's files, including those
    // inside the product's own folders, stay. A junction is left alone.
    Write(install/L"assets\\selection\\sources.json","added-by-update");
    Write(install/L"assets\\selection\\my-mod.png","player");
    Write(install/L"dxwrapper.dll","obsolete-again");
    Write(install/L".ember-update-transaction-v1.json.failed","set-aside-journal");
    // A file only the installed version's manifest names: the helper setup kept
    // may be older than what updates installed since.
    Write(install/L"from-another-version.dll","named-by-manifest");
    Write(install/L"MANIFEST.txt",(Sha256("named-by-manifest")+"  from-another-version.dll\n").c_str());
    // A name that only starts like the updater's own is the player's.
    Write(install/L".ember-update-backups-notes.txt","player");
    Write(root/L"outside\\TRAINING_LAB.md","outside");
    fs::remove_all(install/L"docs"); // The junction takes the shipped doc folder's place.
    std::wstring junction=L"cmd.exe /c mklink /J \""+(install/L"docs").wstring()+L"\" \""+(root/L"outside").wstring()+L"\"";
    STARTUPINFOW junctionStartup{};junctionStartup.cb=sizeof(junctionStartup);PROCESS_INFORMATION junctionChild{};
    CHECK(CreateProcessW(nullptr,&junction[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&junctionStartup,&junctionChild));
    WaitForSingleObject(junctionChild.hProcess,30000);CloseHandle(junctionChild.hThread);CloseHandle(junctionChild.hProcess);
    CHECK(fs::is_regular_file(install/L"docs"/L"TRAINING_LAB.md"));
    // A manifest that is there but cannot be read stops the uninstall before
    // anything goes: it may be all that names a file a later version added.
    HANDLE unreadable=CreateFileW((install/L"MANIFEST.txt").c_str(),GENERIC_READ,FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(unreadable!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::UninstallPackage(install,error) && fs::exists(install/L"Launcher.exe") && fs::exists(install/L"from-another-version.dll"));
    CloseHandle(unreadable);
    // A file in use stops the uninstall; the manifest stays, so the retry
    // still knows the file a later version added is ours.
    HANDLE inUse=CreateFileW((install/L"from-another-version.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(inUse!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::UninstallPackage(install,error) && fs::exists(install/L"MANIFEST.txt"));
    CloseHandle(inUse);
    CHECK(sf4e::launcher::UninstallPackage(install,error));
    CHECK(!fs::exists(install/L"from-another-version.dll") && !fs::exists(install/L"MANIFEST.txt"));
    CHECK(Read(install/L".ember-update-backups-notes.txt")=="player");
    CHECK(!fs::exists(install/L"Launcher.exe") && !fs::exists(install/L"notices") && !fs::exists(install/L"dxwrapper.dll"));
    CHECK(!fs::exists(install/L"assets\\selection\\sources.json") && Read(install/L"assets\\selection\\my-mod.png")=="player");
    CHECK(!fs::exists(install/L".ember-update-backups") && !fs::exists(install/L".ember-update.lock"));
    CHECK(!fs::exists(install/L".ember-update-transaction-v1.json.failed") && Read(install/L".ember-update-transaction-v1.json.new")=="user-journal-name");
    CHECK(Read(install/L"my-replay.bin")=="user" && Read(install/L"d3d9.dll")=="user-owned-proxy");
    CHECK(Read(root/L"outside\\TRAINING_LAB.md")=="outside" && fs::exists(install/L"docs"));
    fs::remove(install/L"docs");
    // Only this uniquely created temporary fixture is removed.
    RemoveTempRoot(root);
    std::cout << "Inventory, upgrade preservation, backup, rollback and uninstall checks passed\n";
}
