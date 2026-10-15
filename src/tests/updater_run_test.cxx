// Updater.exe's run (RunUpdater) with its process boundaries replaced: the
// window it opens for a recovery or an install, the phases that window goes
// through, what it says while the launcher closes and while a restoration
// waits for a held file, and whether the launcher is started again.
#include "../launcher/update/UpdaterRun.hxx"
#include "../common/PackageInventory.hxx"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "temp_root.hxx"
#include "test_support.hxx"

namespace fs = std::filesystem;
using namespace sf4e::launcher;

static void Write(const fs::path& path, const char* text) { fs::create_directories(path.parent_path()); std::ofstream(path,std::ios::binary) << text; }
static std::string Read(const fs::path& path) { std::ifstream input(path,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}}; }
static std::string HashOf(const fs::path& scratch, const char* text) { Write(scratch,text); return Sha256Hex(scratch); }

// An install folder whose update stopped after replacing Launcher.exe and
// before Updater.exe: finishing it restores Launcher.exe from the backup.
// With `replacedAll`, it stopped after both, before the journal was cleared:
// finishing it only clears the journal.
static void Interrupted(const fs::path& root, const fs::path& install, bool replacedAll=false) {
    fs::remove_all(install);
    const auto backup=install/L".ember-update-backups"/L"1-1-1";
    Write(install/L"Launcher.exe","new-launcher"); Write(install/L"Updater.exe",replacedAll?"new-updater":"old-updater");
    Write(backup/L"Launcher.exe","old-launcher"); Write(backup/L"Updater.exe","old-updater");
    const auto scratch=root/L"hash.tmp";
    const nlohmann::json journal={{"schema",1},{"state","prepared"},{"installation",install.u8string()},{"backup",backup.u8string()},
        {"operations",{{{"path","Launcher.exe"},{"existed",true},{"priorSha256",HashOf(scratch,"old-launcher")}},
                       {{"path","Updater.exe"},{"existed",true},{"priorSha256",HashOf(scratch,"old-updater")}}}},
        {"target",{{"Launcher.exe",HashOf(scratch,"new-launcher")},{"Updater.exe",HashOf(scratch,"new-updater")}}}};
    Write(install/UpdateTransactionName,journal.dump().c_str());
}

// This process's Updater window, hidden or not; another process's, such as a
// test running beside this one, is never it.
static HWND OwnWindow() {
    HWND found=nullptr;
    EnumWindows([](HWND window,LPARAM out)->BOOL{
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        wchar_t type[64]={}; GetClassNameW(window,type,64);
        if(pid!=GetCurrentProcessId() || std::wstring(type)!=L"SF4EmberUpdaterProgress") return TRUE;
        *reinterpret_cast<HWND*>(out)=window;
        return FALSE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}
// A package folder of this build's shape: every required file, staged, with
// its inventory and manifest.
static void Staged(const fs::path& package) {
    fs::remove_all(package);
    std::string inventory, manifest;
    for(const auto* path:sf4e::package::Required) {
        Write(package/path,"staged");
        const fs::path relative(path);
        std::string escaped;
        for(const char c:relative.u8string()) { if(c=='\\') escaped+='\\'; escaped+=c; }
        inventory+="SF4E_PACKAGE_REQUIRED(\""+escaped+"\")\n";
    }
    Write(package/L"PackageInventory.inc",inventory.c_str());
    // Every file but the manifest itself.
    for(const auto& entry:fs::recursive_directory_iterator(package))
        if(entry.is_regular_file() && entry.path().filename()!=L"MANIFEST.txt") manifest+=Sha256Hex(entry.path())+"  "+entry.path().lexically_relative(package).u8string()+"\n";
    Write(package/L"MANIFEST.txt",manifest.c_str());
}

// The texts of the Updater window's controls, read from outside it.
static std::vector<std::wstring> WindowTexts() {
    std::vector<std::wstring> texts;
    const HWND frame=OwnWindow();
    if(!frame) return texts;
    EnumChildWindows(frame,[](HWND child,LPARAM list)->BOOL{
        wchar_t text[256]={}; GetWindowTextW(child,text,256);
        reinterpret_cast<std::vector<std::wstring>*>(list)->push_back(text);
        return TRUE;
    },reinterpret_cast<LPARAM>(&texts));
    return texts;
}
static bool Showing(const std::wstring& wanted) {
    for(const auto& text:WindowTexts()) if(text==wanted) return true;
    return false;
}
// Waits up to two seconds for the window to show `wanted`.
static bool Shows(const std::wstring& wanted) {
    for(int i=0; i<200 && !Showing(wanted); ++i) Sleep(10);
    return Showing(wanted);
}
static std::wstring Clock() {
    for(const auto& text:WindowTexts()) if(text.size()>8 && text.compare(text.size()-8,8,L" elapsed")==0) return text;
    return {};
}
static std::wstring English(UpdaterNotice notice) { return UpdaterNotices[static_cast<std::size_t>(notice)].english; }

struct Host {
    UpdaterHost host;
    std::vector<std::wstring> started;
    std::vector<std::uint32_t> waited;
    bool launcherExits=true;
    std::function<void()> whileWaiting;
    // Called when the run logs that its recovery is done, while the window
    // still shows the phase it ended in.
    std::function<void()> whenRecovered;
    // The phases the window was set to, by locale key, each once in a row.
    std::vector<std::string> phases;
    std::mutex phasesLock;
    Host() {
        host.window.visible=false;
        host.window.entered=[this](const WindowPhase& phase){
            std::string key;
            if(const auto* step=std::get_if<StepReport>(&phase)) key=InstallerSteps[InstallerStepOf(step->stage)].nameKey;
            if(const auto* notice=std::get_if<UpdaterNotice>(&phase)) key=UpdaterNotices[static_cast<std::size_t>(*notice)].key;
            std::lock_guard<std::mutex> lock(phasesLock);
            if(phases.empty() || phases.back()!=key) phases.push_back(key);
        };
        host.log=[this](const char* line){ if(whenRecovered && std::string(line)=="Update recovery complete") whenRecovered(); };
        host.waitForLauncher=[this](std::uint32_t pid){ waited.push_back(pid); if(whileWaiting) whileWaiting(); return launcherExits; };
        host.startLauncher=[this](const wchar_t*,const wchar_t* arguments){ started.push_back(arguments?arguments:L"<none>"); return true; };
    }
};

int main() {
    const auto root=MakeTempRoot(L"ember-updater-run-"), install=root/L"install";
    fs::create_directories(root);
    UpdaterArguments arguments; arguments.installDir=install.wstring(); arguments.recoverOnly=true; arguments.waitPid=4242;

    // The recovery the launcher asks for at startup: its window is open while
    // the launcher closes; then its restoration is shown, and the wait for a
    // file another program holds is named while the clock goes on. The file
    // is let go only then, the restoration finishes, and Ember is started.
    Interrupted(root,install);
    testing::InUseWaitMilliseconds=5000;
    HANDLE held=CreateFileW((install/L"Launcher.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(held!=INVALID_HANDLE_VALUE);
    Host recovery;
    bool waitShown=false, restoringAtEnd=false;
    HWND window=nullptr;
    recovery.whileWaiting=[&]{
        window=OwnWindow();
        waitShown=window && Shows(English(UpdaterNotice::Recovering)+L"\n"+English(UpdaterNotice::Reopens)) && Shows(English(UpdaterNotice::WaitingForLauncher));
    };
    recovery.whenRecovered=[&]{ restoringAtEnd=Shows(English(UpdaterNotice::Restoring)); };
    std::atomic<bool> restorationShown{false}, fileShown{false}, clockMoved{false};
    std::thread release([&]{
        restorationShown=Shows(English(UpdaterNotice::Restoring));
        for(int i=0; i<500 && !fileShown; ++i) { fileShown=Showing(FillNotice(English(UpdaterNotice::WaitingForFile),L"Launcher.exe")); Sleep(10); }
        const auto clock=Clock();
        for(int i=0; i<250 && fileShown && !clockMoved; ++i) { Sleep(10); clockMoved=!clock.empty() && Clock()!=clock; }
        // Still restoring while it waits: the wait is beside the phase.
        restorationShown=restorationShown && Showing(English(UpdaterNotice::Restoring));
        CloseHandle(held);
    });
    const int code=RunUpdater(arguments,recovery.host);
    release.join();
    testing::InUseWaitMilliseconds=0;
    CHECK(waitShown && restorationShown && fileShown && clockMoved && restoringAtEnd);
    CHECK(code==0 && recovery.waited==std::vector<std::uint32_t>{4242} && recovery.started==std::vector<std::wstring>{L"<none>"});
    CHECK(Read(install/L"Launcher.exe")=="old-launcher" && !fs::exists(install/UpdateTransactionName));
    // The run closed the window it opened.
    CHECK(!IsWindow(window) && !OwnWindow());

    // A recovery that needs no restoration, its update already complete:
    // once the launcher has closed, the phase is finishing the update, not
    // waiting for the launcher, until the end.
    Interrupted(root,install,true);
    Host complete;
    bool finishingShown=false;
    complete.whenRecovered=[&]{
        finishingShown=Shows(English(UpdaterNotice::Recovering)) && !Showing(English(UpdaterNotice::WaitingForLauncher));
    };
    CHECK(RunUpdater(arguments,complete.host)==0 && finishingShown && complete.started==std::vector<std::wstring>{L"<none>"});
    CHECK(Read(install/L"Launcher.exe")=="new-launcher" && !fs::exists(install/UpdateTransactionName));
    CHECK((complete.phases==std::vector<std::string>{"update.notice.waiting_launcher","update.notice.recovering"}));
    CHECK((recovery.phases==std::vector<std::string>{"update.notice.waiting_launcher","update.notice.recovering","update.notice.restoring"}));

    // An install over a folder that holds an update left unfinished, already
    // complete: once the launcher has closed, the window goes from that wait
    // to the look over the folder, then to the install's steps from its
    // first, which is announced before the package is walked; the files are
    // the package's and Ember is started.
    Interrupted(root,install,true);
    const auto staging=root/L"staging";
    Staged(staging);
    UpdaterArguments fresh; fresh.installDir=install.wstring(); fresh.stagingDir=staging.wstring(); fresh.waitPid=4242;
    Host installing;
    CHECK(RunUpdater(fresh,installing.host)==0 && installing.started==std::vector<std::wstring>{L"<none>"});
    CHECK(Read(install/L"Launcher.exe")=="staged" && !fs::exists(install/UpdateTransactionName));
    CHECK((installing.phases==std::vector<std::string>{"update.notice.waiting_launcher","update.notice.recovering","update.stage.preparing",
        "update.stage.comparing","update.stage.backing_up","update.stage.replacing","update.stage.confirming"}));
    // With nothing unfinished, the first step follows the launcher wait.
    Host again;
    CHECK(RunUpdater(fresh,again.host)==0 && again.phases.size()>=2);
    CHECK(again.phases[0]=="update.notice.waiting_launcher" && again.phases[1]=="update.stage.preparing");

    // Started by hand, without -WaitPid: the same work, and no launcher.
    Interrupted(root,install);
    arguments.waitPid=0;
    Host byHand;
    CHECK(RunUpdater(arguments,byHand.host)==0 && byHand.started.empty() && Read(install/L"Launcher.exe")=="old-launcher");

    // A recovery that fails opens the launcher on its Updates page.
    Interrupted(root,install);
    Write(install/UpdateTransactionName,"not a journal");
    arguments.waitPid=4242;
    Host failing;
    CHECK(RunUpdater(arguments,failing.host)==1 && failing.started==std::vector<std::wstring>{L"--updates --update-error"});

    // A launcher that never closes stops the recovery before anything
    // changes, and the running launcher is left as it is.
    Interrupted(root,install);
    Host stuck; stuck.launcherExits=false;
    CHECK(RunUpdater(arguments,stuck.host)==1 && stuck.started.empty());
    CHECK(Read(install/L"Launcher.exe")=="new-launcher" && fs::exists(install/UpdateTransactionName));

    // An install whose launcher never closes is reported by a launcher on
    // the Updates page.
    UpdaterArguments update; update.installDir=install.wstring(); update.stagingDir=root.wstring(); update.waitPid=4242;
    CHECK(RunUpdater(update,stuck.host)==1 && stuck.started==std::vector<std::wstring>{L"--updates --update-error"});
    RemoveTempRoot(root);
    return 0;
}
