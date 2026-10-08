#include "../launcher/update/PackageInstaller.hxx"
#include "../launcher/update/UpdateHandoff.hxx"
#include "../launcher/update/ProgressWindow.hxx"
#include "../common/PackageInventory.hxx"
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <cctype>
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
// PackageInventory.inc declaring these paths, in the form the real one uses.
std::string Escaped(const std::wstring& path) { std::string text; for (wchar_t c : path) { if (c == L'\\') text += '\\'; text += static_cast<char>(c); } return text; }
void Inventory(const fs::path& package, const std::vector<std::wstring>& required, const std::vector<std::wstring>& obsolete) {
    std::string lines;
    for (const auto& path : required) lines += "SF4E_PACKAGE_REQUIRED(\"" + Escaped(path) + "\")\n";
    for (const auto& path : obsolete) lines += "SF4E_PACKAGE_OBSOLETE(\"" + Escaped(path) + "\")\n";
    Write(package/L"PackageInventory.inc", lines.c_str());
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
// A package of this build's shape: every required file with this content, its
// inventory and its manifest.
void Package(const fs::path& package, const char* content) {
    std::vector<std::wstring> required, obsolete;
    for (const auto* path : sf4e::package::Required) { Write(package/path, content); required.push_back(path); }
    for (const auto* path : sf4e::package::Obsolete) obsolete.push_back(path);
    Inventory(package, required, obsolete); Manifest(package);
}
HANDLE progressThread=nullptr;
HANDLE WINAPI TrackProgressThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack, LPTHREAD_START_ROUTINE run, LPVOID parameter, DWORD flags, LPDWORD id) {
    const HANDLE thread=CreateThread(attributes,stack,run,parameter,flags,id);
    if(thread) DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&progressThread,SYNCHRONIZE,FALSE,0);
    return thread;
}
HANDLE WINAPI FailProgressThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD) { return nullptr; }
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
        // What the updater will hold it to as well, so packaging and updating agree.
        CHECK(sf4e::launcher::ValidatePackageFolder(package,error));
        std::cout << "Actual package accepted by the native updater inventory and validation\n";
        return 0;
    }
    const auto root = MakeTempRoot(L"ember-upgrade-test-");
    const auto staging = root/L"staging", install = root/L"install";
    fs::create_directories(staging); fs::create_directories(install);
    Package(staging,"new");
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
    std::uint64_t checked=0,named=0;
    CHECK(sf4e::launcher::ValidatePackageFolder(staging,error,[&](std::uint64_t done,std::uint64_t total){checked=done;named=total;return true;}));
    CHECK(named>0 && checked==named);
    // A pass that is told to stop fails as cancelled, from whichever of its
    // threads heard it.
    CHECK(!sf4e::launcher::ValidatePackageFolder(staging,error,[](std::uint64_t,std::uint64_t){return false;}) && error=="Cancelled");
    // Cancel arrives after the last preparation report was accepted. The
    // handoff polls the same counters and never calls the process starter.
    bool cancelRequested=false, spawned=false;
    const auto preparing=[&](std::uint64_t done,std::uint64_t total) {
        checked=done; named=total;
        if(cancelRequested) return false;
        if(done==total) cancelRequested=true;
        return true;
    };
    CHECK(sf4e::launcher::ValidatePackageFolder(staging,error,preparing) && cancelRequested && checked==named);
    CHECK(!sf4e::launcher::HandoffPreparedUpdate(preparing,checked,named,[&]{spawned=true;}) && !spawned);
    CHECK(sf4e::launcher::HandoffPreparedUpdate({},checked,named,[&]{spawned=true;}) && spawned);
    // A startup timeout stops a worker still behind its gate, so it cannot
    // create a late window. Both successful and failed starts own no handles
    // after closing; a normal close joins before destroying the owner.
    using sf4e::launcher::ProgressWindow;
    ProgressWindow::Options windowOptions;
    windowOptions.visible=false; windowOptions.createThread=TrackProgressThread;
    windowOptions.timeout=0; windowOptions.startupGate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(windowOptions.startupGate!=nullptr);
    {
        ProgressWindow delayed(L"Installing",windowOptions);
        CHECK(delayed.Window()==nullptr && progressThread!=nullptr && WaitForSingleObject(progressThread,0)==WAIT_OBJECT_0);
        SetEvent(windowOptions.startupGate);
        CHECK(delayed.Window()==nullptr);
    }
    CloseHandle(progressThread); progressThread=nullptr;
    CloseHandle(windowOptions.startupGate); windowOptions.startupGate=nullptr;
    windowOptions.createThread=FailProgressThread;
    { ProgressWindow failed(L"Installing",windowOptions); CHECK(failed.Window()==nullptr); }
    windowOptions.createThread=TrackProgressThread; windowOptions.timeout=5000;
    HWND progressBar=nullptr;
    {
        ProgressWindow normal(L"Installing",windowOptions);
        progressBar=normal.Window();
        CHECK(progressBar!=nullptr && IsWindow(progressBar) && progressThread!=nullptr);
        CHECK(SendMessageW(progressBar,PBM_GETRANGE,FALSE,0)==ProgressWindow::Range);
        PostMessageW(progressBar,PBM_SETPOS,ProgressWindow::Range,0);
    }
    CHECK(!IsWindow(progressBar) && WaitForSingleObject(progressThread,0)==WAIT_OBJECT_0);
    CloseHandle(progressThread); progressThread=nullptr;
    // This version ships a doc and a selection asset the older one below lacks.
    Write(staging/L"docs\\TRAINING_LAB.md","new"); Write(staging/L"assets\\selection\\sources.json","new"); Manifest(staging);
    // The bar only goes forward, to a total that is fixed from the first
    // step: here nearly every file changes and two are removed.
    std::uint64_t steps=0,lastDone=0,firstTotal=0,lastTotal=0; bool forward=true;
    const auto watch=[&](std::uint64_t done,std::uint64_t total){
        if(!steps++) firstTotal=total;
        forward=forward&&done>=lastDone&&total==firstTotal; lastDone=done; lastTotal=total; return true;
    };
    CHECK(sf4e::launcher::InstallPackage(staging,install,error,watch));
    CHECK(steps>0 && lastTotal>0 && lastDone==lastTotal && forward);
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
    // The same package again writes nothing: the backup set of the update
    // before stays, and no file is rewritten.
    const auto launcherWritten=fs::last_write_time(install/L"Launcher.exe");
    steps=lastDone=0;
    CHECK(sf4e::launcher::InstallPackage(staging,install,error,watch));
    CHECK(steps>0 && lastDone==lastTotal && forward);
    CHECK(fs::last_write_time(install/L"Launcher.exe")==launcherWritten && !fs::exists(install/L".ember-update-transaction-v1.json"));
    backed=false;
    for (const auto& item : fs::recursive_directory_iterator(install/L".ember-update-backups")) backed=backed||item.path().filename()==L"Qt6Core.dll";
    CHECK(backed);
    // H-013: another update replaces the backup set instead of adding one,
    // and backs up only the file it changes.
    Write(staging/L"Launcher.exe","newer"); Manifest(staging);
    // Told to stop before anything is written, it changes nothing.
    CHECK(!sf4e::launcher::InstallPackage(staging,install,error,[](std::uint64_t,std::uint64_t){return false;}) && Read(install/L"Launcher.exe")=="new");
    CHECK(sf4e::launcher::InstallPackage(staging,install,error));
    CHECK(Read(install/L"Launcher.exe")=="newer");
    CHECK(std::distance(fs::directory_iterator(install/L".ember-update-backups"), fs::directory_iterator()) == 1);
    backed=false; bool unchangedBacked=false;
    for (const auto& item : fs::recursive_directory_iterator(install/L".ember-update-backups")) {
        if (item.path().filename() == L"Launcher.exe") backed = Read(item.path()) == "new";
        if (item.path().filename() == L"Updater.exe") unchangedBacked = true;
    }
    CHECK(backed && !unchangedBacked);
    Write(staging/L"Launcher.exe","new"); Manifest(staging);
    CHECK(sf4e::launcher::InstallPackage(staging,install,error) && Read(install/L"Launcher.exe")=="new");
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
    std::vector<std::wstring> olderRequired;
    for (const auto* path : sf4e::package::Required) if (std::wstring(path).find(L"iscord")==std::wstring::npos) { Write(older/path,"older"); olderRequired.push_back(path); }
    Write(older/L"dxwrapper.dll","older-display-wrapper"); olderRequired.push_back(L"dxwrapper.dll");
    Inventory(older,olderRequired,{}); Manifest(older);
    // Windows PowerShell writes its digests in uppercase; the package still
    // validates, installs and passes the final check below.
    { std::string upper=Read(older/L"MANIFEST.txt"); for(size_t at=0;at<upper.size();at=upper.find('\n',at)+1){ for(size_t i=at;i<at+64&&i<upper.size();++i) upper[i]=static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i]))); if(upper.find('\n',at)==std::string::npos) break; } Write(older/L"MANIFEST.txt",upper.c_str()); }
    CHECK(Read(older/L"MANIFEST.txt").find('A')!=std::string::npos || Read(older/L"MANIFEST.txt").find('F')!=std::string::npos);
    CHECK(sf4e::launcher::ValidatePackageFolder(older,error));
    // Matching a manifest is not completeness: a package short of a file its
    // own inventory requires, or carrying one it calls obsolete, is refused.
    const auto trimmed=root/L"trimmed"; fs::copy(staging,trimmed,fs::copy_options::recursive);
    fs::remove(trimmed/L"sf4-net.exe"); Manifest(trimmed);
    CHECK(!sf4e::launcher::ValidatePackageFolder(trimmed,error) && error.find("sf4-net.exe")!=std::string::npos);
    fs::copy_file(staging/L"sf4-net.exe",trimmed/L"sf4-net.exe"); Write(trimmed/L"Qt6Core.dll","carried"); Manifest(trimmed);
    CHECK(!sf4e::launcher::ValidatePackageFolder(trimmed,error));
    // The inventory reader keeps every declaration the compiler keeps, or
    // refuses the file: an indented one still requires its file, and a line
    // it cannot follow is an error, never a requirement quietly dropped.
    fs::remove(trimmed/L"Qt6Core.dll"); fs::remove(trimmed/L"sf4-net.exe");
    const auto inventoryText=Read(trimmed/L"PackageInventory.inc");
    const auto rewritten=[&](const std::string& from,const std::string& to){
        std::string text=inventoryText; text.replace(text.find(from),from.size(),to);
        Write(trimmed/L"PackageInventory.inc",text.c_str()); Manifest(trimmed);
        return !sf4e::launcher::ValidatePackageFolder(trimmed,error);
    };
    const std::string netLine="SF4E_PACKAGE_REQUIRED(\"sf4-net.exe\")";
    CHECK(rewritten(netLine,"  "+netLine+"  ") && error.find("sf4-net.exe")!=std::string::npos);
    CHECK(rewritten(netLine,"SF4E_PACKAGE_REQUIRED( \"sf4-net.exe\")") && error.find("inventory")!=std::string::npos);
    CHECK(rewritten(netLine,"/* "+netLine+" */") && error.find("inventory")!=std::string::npos);
    // A path literal means what the compiler reads: any escape other than a
    // doubled backslash, and any "." or ".." part, is refused.
    CHECK(rewritten(netLine,"SF4E_PACKAGE_REQUIRED(\"sf4-net.exe\\0/../../Launcher.exe\")") && error.find("inventory")!=std::string::npos);
    CHECK(rewritten(netLine,"SF4E_PACKAGE_REQUIRED(\"sf4-net.ex\\x65\")") && error.find("inventory")!=std::string::npos);
    CHECK(rewritten(netLine,"SF4E_PACKAGE_REQUIRED(\"notices\\\\..\\\\sf4-net.exe\")") && error.find("inventory")!=std::string::npos);
    // A bare CR ends a line for other readers: one inside a comment would hide
    // the declaration after it, so it is refused.
    CHECK(rewritten(netLine,"// note\r"+netLine) && error.find("inventory")!=std::string::npos);
    CHECK(sf4e::launcher::InstallPackage(older,install,error));
    CHECK(Read(install/L"Launcher.exe")=="older" && Read(install/L"dxwrapper.dll")=="older-display-wrapper");
    CHECK(!fs::exists(install/L"docs\\TRAINING_LAB.md") && !fs::exists(install/L"assets\\selection\\sources.json") && !fs::exists(install/L"docs"));
    CHECK(!fs::exists(install/L"ember-discord.exe") && !fs::exists(install/L"notices\\Discord-SDK.txt"));
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
    // An installed manifest that is there but cannot be read stops the
    // update before anything changes: replacing it would lose for good which
    // files this folder owns.
    const auto installedManifest=Read(install/L"MANIFEST.txt");
    Write(install/L"MANIFEST.txt",(installedManifest+"not a manifest line\n").c_str());
    CHECK(!sf4e::launcher::InstallPackage(older,install,error) && Read(install/L"Launcher.exe")=="new");
    Write(install/L"MANIFEST.txt",installedManifest.c_str());
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
    Package(crashStaging,"target");
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

    // An update leaves alone the files that already are the package's: they
    // are in the journal's target and in none of its operations, and have no
    // backup. Here two files and the manifest change, and the process ends
    // after the first replacement.
    const auto sparseRoot=root/L"sparse", sparseStaging=sparseRoot/L"staging", sparseInstall=sparseRoot/L"install";
    fs::create_directories(sparseStaging); fs::create_directories(sparseInstall);
    Package(sparseStaging,"first");
    CHECK(sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error));
    Write(sparseStaging/L"Launcher.exe","second"); Write(sparseStaging/L"Updater.exe","second"); Manifest(sparseStaging);
    std::wstring sparseCommand=L"\""+fs::absolute(argv[0]).wstring()+L"\" --crash-child \""+sparseRoot.wstring()+L"\"";
    STARTUPINFOW sparseStartup{};sparseStartup.cb=sizeof(sparseStartup);PROCESS_INFORMATION sparseChild{};
    SetEnvironmentVariableW(L"SF4E_UPDATE_TEST_TERMINATE_AFTER",L"1");
    CHECK(CreateProcessW(nullptr,&sparseCommand[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&sparseStartup,&sparseChild));
    SetEnvironmentVariableW(L"SF4E_UPDATE_TEST_TERMINATE_AFTER",nullptr);
    WaitForSingleObject(sparseChild.hProcess,30000);DWORD sparseExit=0;GetExitCodeProcess(sparseChild.hProcess,&sparseExit);
    CloseHandle(sparseChild.hThread);CloseHandle(sparseChild.hProcess);CHECK(sparseExit==86);
    const auto sparseJournalPath=sparseInstall/L".ember-update-transaction-v1.json", sparseFailed=sparseInstall/L".ember-update-transaction-v1.json.failed";
    const auto sparseJournal=Read(sparseJournalPath);
    CHECK(nlohmann::json::parse(sparseJournal)["operations"].size()==3 && nlohmann::json::parse(sparseJournal)["target"].size()>3);
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"Updater.exe")=="first");
    // Read failures leave retryable evidence in place, whether it is the
    // journal itself or a backup needed by this half-applied update.
    HANDLE unreadableJournal=CreateFileW(sparseJournalPath.c_str(),GENERIC_READ,FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(unreadableJournal!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error) && error.find("Cannot read")!=std::string::npos);
    CloseHandle(unreadableJournal);
    CHECK(Read(sparseJournalPath)==sparseJournal && !fs::exists(sparseFailed) && Read(sparseInstall/L"Launcher.exe")=="second");
    const auto sparseBackup=fs::u8path(nlohmann::json::parse(sparseJournal)["backup"].get<std::string>());
    HANDLE unreadableBackup=CreateFileW((sparseBackup/L"Launcher.exe").c_str(),GENERIC_READ,FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(unreadableBackup!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error));
    CloseHandle(unreadableBackup);
    CHECK(Read(sparseJournalPath)==sparseJournal && !fs::exists(sparseFailed) && Read(sparseInstall/L"Launcher.exe")=="second");
    // Recovery restores what the update changed and leaves the rest alone.
    const auto untouched=fs::last_write_time(sparseInstall/L"sf4-net.exe");
    CHECK(sf4e::launcher::RecoverPackage(sparseInstall,error));
    CHECK(Read(sparseInstall/L"Launcher.exe")=="first" && !fs::exists(sparseJournalPath) && fs::last_write_time(sparseInstall/L"sf4-net.exe")==untouched);
    // One of the files left alone that is gone by then (an antivirus
    // quarantine, say) has no backup. The half-applied update is still
    // restored, and the failure names the file, once.
    Write(sparseInstall/L"Launcher.exe","second"); fs::remove(sparseInstall/L"sf4-net.exe");
    Write(sparseJournalPath,sparseJournal.c_str());
    HANDLE blockedMissingRestore=CreateFileW((sparseInstall/L"Launcher.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(blockedMissingRestore!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error));
    CHECK(nlohmann::json::parse(Read(sparseJournalPath))["state"]=="rolling-back" && !fs::exists(sparseFailed));
    CloseHandle(blockedMissingRestore);
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error) && error.find("sf4-net.exe")!=std::string::npos);
    CHECK(Read(sparseInstall/L"Launcher.exe")=="first" && !fs::exists(sparseJournalPath) && fs::exists(sparseFailed));
    CHECK(sf4e::launcher::RecoverPackage(sparseInstall,error));
    // The same when every operation had finished: the update is not undone,
    // and is not reported as whole either.
    fs::remove(sparseFailed);
    for(const auto* changed:{L"Launcher.exe",L"Updater.exe",L"MANIFEST.txt"}) fs::copy_file(sparseStaging/changed,sparseInstall/changed,fs::copy_options::overwrite_existing);
    Write(sparseJournalPath,sparseJournal.c_str());
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error) && error.find("sf4-net.exe")!=std::string::npos);
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && !fs::exists(sparseJournalPath) && fs::exists(sparseFailed));
    // Installing the update again writes the missing file.
    CHECK(sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error) && Read(sparseInstall/L"sf4-net.exe")=="first");
    // Completed operations do not establish a complete target. A skipped
    // edit means the folder was replaced since; it survives with no rollback.
    fs::remove(sparseFailed);
    Write(sparseInstall/L"sf4-net.exe","edited-later");
    Write(sparseJournalPath,sparseJournal.c_str());
    CHECK(sf4e::launcher::RecoverPackage(sparseInstall,error) && error.empty());
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"sf4-net.exe")=="edited-later");
    CHECK(!fs::exists(sparseJournalPath) && fs::exists(sparseFailed));
    // An unreadable skipped file preserves both the journal and all bytes
    // for retry, even if every operation already reached its target.
    fs::remove(sparseFailed); Write(sparseInstall/L"sf4-net.exe","first");
    Write(sparseJournalPath,sparseJournal.c_str());
    HANDLE unreadableSkipped=CreateFileW((sparseInstall/L"sf4-net.exe").c_str(),GENERIC_READ,FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(unreadableSkipped!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error) && error.find("sf4-net.exe")!=std::string::npos);
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseJournalPath)==sparseJournal && !fs::exists(sparseFailed));
    CloseHandle(unreadableSkipped);
    CHECK(sf4e::launcher::RecoverPackage(sparseInstall,error) && error.empty() && Read(sparseInstall/L"sf4-net.exe")=="first");
    CHECK(!fs::exists(sparseJournalPath) && !fs::exists(sparseFailed));
    // Cancellation after the first replacement restores only operations.
    // A concurrent edit of a skipped file survives that restoration.
    Write(sparseStaging/L"Launcher.exe","cancelled-target"); Manifest(sparseStaging);
    const auto priorSparseManifest=Read(sparseInstall/L"MANIFEST.txt");
    bool editedSkipped=false;
    CHECK(!sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error,[&](std::uint64_t,std::uint64_t) {
        if(!fs::exists(sparseJournalPath) || Read(sparseInstall/L"Launcher.exe")!="cancelled-target") return true;
        Write(sparseInstall/L"sf4-net.exe","edited-during-install"); editedSkipped=true; return false;
    }));
    CHECK(editedSkipped && error=="Cancelled. Previous files restored.");
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"Updater.exe")=="second");
    CHECK(Read(sparseInstall/L"MANIFEST.txt")==priorSparseManifest && Read(sparseInstall/L"sf4-net.exe")=="edited-during-install");
    CHECK(!fs::exists(sparseJournalPath) && !fs::exists(sparseFailed));
    Write(sparseInstall/L"sf4-net.exe","first");
    // A replaced destination held without delete sharing blocks restoration.
    // The rollback phase stays durable until every operation is restored.
    Write(sparseStaging/L"Updater.exe","cancelled-updater"); Manifest(sparseStaging);
    HANDLE blockedRestore=INVALID_HANDLE_VALUE;
    CHECK(!sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error,[&](std::uint64_t,std::uint64_t) {
        if(!fs::exists(sparseJournalPath) || Read(sparseInstall/L"Launcher.exe")!="cancelled-target") return true;
        Write(sparseInstall/L"sf4-net.exe","edited-during-rollback");
        blockedRestore=CreateFileW((sparseInstall/L"Launcher.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        CHECK(blockedRestore!=INVALID_HANDLE_VALUE); return false;
    }));
    CHECK(error.find("Cancelled. Automatic restore incomplete")!=std::string::npos);
    const auto rollbackJournal=nlohmann::json::parse(Read(sparseJournalPath));
    CHECK(rollbackJournal["state"]=="rolling-back" && rollbackJournal["operations"].size()==3 && !fs::exists(sparseFailed));
    CHECK(Read(sparseInstall/L"Launcher.exe")=="cancelled-target" && Read(sparseInstall/L"sf4-net.exe")=="edited-during-rollback");
    CHECK(!sf4e::launcher::RecoverPackage(sparseInstall,error));
    CHECK(nlohmann::json::parse(Read(sparseJournalPath))==rollbackJournal && !fs::exists(sparseFailed));
    CloseHandle(blockedRestore);
    CHECK(sf4e::launcher::RecoverPackage(sparseInstall,error) && error.empty());
    for(const auto& operation:rollbackJournal["operations"]) {
        const auto path=sparseInstall/fs::u8path(operation["path"].get<std::string>());
        CHECK(operation["existed"].get<bool>() && sf4e::launcher::Sha256Hex(path)==operation["priorSha256"].get<std::string>());
    }
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"Updater.exe")=="second" && Read(sparseInstall/L"MANIFEST.txt")==priorSparseManifest);
    CHECK(Read(sparseInstall/L"sf4-net.exe")=="edited-during-rollback" && !fs::exists(sparseJournalPath) && !fs::exists(sparseFailed));
    // An interruption after one restoration leaves both prior and target
    // bytes. Recovery resumes rollback and leaves an edited skipped file alone.
    const auto rollbackBackup=fs::u8path(rollbackJournal["backup"].get<std::string>());
    for(const auto& operation:rollbackJournal["operations"]) {
        const auto relative=fs::u8path(operation["path"].get<std::string>());
        fs::copy_file(sparseStaging/relative,sparseInstall/relative,fs::copy_options::overwrite_existing);
    }
    fs::copy_file(rollbackBackup/L"Launcher.exe",sparseInstall/L"Launcher.exe",fs::copy_options::overwrite_existing);
    Write(sparseJournalPath,rollbackJournal.dump().c_str());
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"Updater.exe")=="cancelled-updater");
    HANDLE restoredLauncher=CreateFileW((sparseInstall/L"Launcher.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(restoredLauncher!=INVALID_HANDLE_VALUE);
    CHECK(sf4e::launcher::RecoverPackage(sparseInstall,error) && error.empty());
    CloseHandle(restoredLauncher);
    for(const auto& operation:rollbackJournal["operations"]) {
        const auto path=sparseInstall/fs::u8path(operation["path"].get<std::string>());
        CHECK(sf4e::launcher::Sha256Hex(path)==operation["priorSha256"].get<std::string>());
    }
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"Updater.exe")=="second" && Read(sparseInstall/L"MANIFEST.txt")==priorSparseManifest);
    CHECK(Read(sparseInstall/L"sf4-net.exe")=="edited-during-rollback" && !fs::exists(sparseJournalPath) && !fs::exists(sparseFailed));
    Write(sparseInstall/L"sf4-net.exe","first");
    // The same edit after a replacement can fail final verification without
    // cancellation; restoration still returns recorded files to prior bytes.
    editedSkipped=false;
    CHECK(!sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error,[&](std::uint64_t,std::uint64_t) {
        if(!editedSkipped && fs::exists(sparseJournalPath) && Read(sparseInstall/L"Launcher.exe")=="cancelled-target") {
            Write(sparseInstall/L"sf4-net.exe","verification-edit"); editedSkipped=true;
        }
        return true;
    }));
    CHECK(editedSkipped && error=="Installed update verification failed. Previous files restored.");
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && Read(sparseInstall/L"MANIFEST.txt")==priorSparseManifest);
    CHECK(Read(sparseInstall/L"sf4-net.exe")=="verification-edit" && !fs::exists(sparseJournalPath) && !fs::exists(sparseFailed));
    Write(sparseInstall/L"sf4-net.exe","first");
    // An install that fails part way undoes its own work the same way: the
    // file it replaced is restored, the ones it left alone are not touched.
    Write(sparseStaging/L"Launcher.exe","third"); Manifest(sparseStaging);
    const auto leftAlone=fs::last_write_time(sparseInstall/L"sf4-net.exe");
    HANDLE heldManifest=CreateFileW((sparseInstall/L"MANIFEST.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(heldManifest!=INVALID_HANDLE_VALUE);
    CHECK(!sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error) && error.find("restored")!=std::string::npos);
    CloseHandle(heldManifest);
    CHECK(Read(sparseInstall/L"Launcher.exe")=="second" && !fs::exists(sparseJournalPath) && fs::last_write_time(sparseInstall/L"sf4-net.exe")==leftAlone);
    CHECK(sf4e::launcher::InstallPackage(sparseStaging,sparseInstall,error) && Read(sparseInstall/L"Launcher.exe")=="third");
    // Uninstall removes what the installed manifest or the inventory names and
    // the updater's state; the player's files, including those inside the
    // product's own folders, stay. A junction is left alone.
    Write(install/L"assets\\selection\\sources.json","added-by-update");
    Write(install/L"assets\\selection\\my-mod.png","player");
    // Selection art is Ember's only when the manifest names it: a cutout it
    // names goes, a photograph at an allowed path that no package shipped stays.
    Write(install/L"assets\\selection\\RYU\\costume-0\\color-0-cutout.png","shipped-cutout");
    Write(install/L"assets\\selection\\RYU\\costume-0\\color-0.jpg","player-photo");
    Write(install/L"dxwrapper.dll","obsolete-again");
    Write(install/L".ember-update-transaction-v1.json.failed","set-aside-journal");
    // A file only the installed version's manifest names: the helper setup kept
    // may be older than what updates installed since.
    Write(install/L"from-another-version.dll","named-by-manifest");
    Write(install/L"MANIFEST.txt",(Sha256("named-by-manifest")+"  from-another-version.dll\n"+
        Sha256("added-by-update")+"  assets\\selection\\sources.json\n"+
        Sha256("shipped-cutout")+"  assets\\selection\\RYU\\costume-0\\color-0-cutout.png\n").c_str());
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
    CHECK(!fs::exists(install/L"assets\\selection\\RYU\\costume-0\\color-0-cutout.png") && Read(install/L"assets\\selection\\RYU\\costume-0\\color-0.jpg")=="player-photo");
    CHECK(!fs::exists(install/L".ember-update-backups") && !fs::exists(install/L".ember-update.lock"));
    CHECK(!fs::exists(install/L".ember-update-transaction-v1.json.failed") && Read(install/L".ember-update-transaction-v1.json.new")=="user-journal-name");
    CHECK(Read(install/L"my-replay.bin")=="user" && Read(install/L"d3d9.dll")=="user-owned-proxy");
    CHECK(Read(root/L"outside\\TRAINING_LAB.md")=="outside" && fs::exists(install/L"docs"));
    fs::remove(install/L"docs");
    // Only this uniquely created temporary fixture is removed.
    RemoveTempRoot(root);
    std::cout << "Inventory, upgrade preservation, backup, rollback and uninstall checks passed\n";
}
