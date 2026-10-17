#include "../platform/LauncherLinks.hxx"
#include "../platform/LauncherInstance.hxx"
#include "test_support.hxx"
using namespace sf4e::platform;
static const char* Bridge="brg_0dbc0598-2312-4ce3-9df8-e160330565e6";
static const char* Match="emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
static const char* Room="0123456789abcdef0123456789abcdef";
int wmain(int argc,wchar_t** argv) {
    const std::wstring scope=argc>=2?argv[1]:L".HomeTest."+std::to_wstring(GetCurrentProcessId());
    const auto name=[&](const wchar_t* base){return std::wstring(base)+scope;};
    if(argc==3){
        LauncherLinks retained(scope);CHECK(retained.Open());
        HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,(scope+L".release").c_str());CHECK(release);
        CHECK(WaitForSingleObject(release,10000)==WAIT_OBJECT_0);CloseHandle(release);
        return 0;
    }
    if(argc==2){
        LauncherInstance second;CHECK(!second.Acquire(scope));
        CHECK(DeliverJoinLink("7K3M0X1RT9PZ",name(JoinLinkSectionName()).c_str(),name(JoinLinkEventName()).c_str()));
        CHECK(DeliverMatchLink(sf4e::tournament_link::Checked(Bridge,Match),name(MatchLinkSectionName()).c_str(),name(MatchLinkEventName()).c_str()));
        CHECK(DeliverPublicRoomLink(sf4e::tournament_link::CheckedRoom(Bridge,Room),name(PublicRoomLinkSectionName()).c_str(),name(PublicRoomLinkEventName()).c_str()));
        CHECK(DeliverConnectLink(Bridge,name(ConnectLinkSectionName()).c_str(),name(ConnectLinkEventName()).c_str()));
        CHECK(DeliverReplayLink("private-fixture.sf4r",name(ReplayLinkSectionName()).c_str(),name(ReplayLinkEventName()).c_str()));
        CHECK(DeliverDiscordLaunch(name(DiscordLaunchSectionName()).c_str(),name(DiscordLaunchEventName()).c_str()));
        return 0;
    }
    {
        LauncherInstance owner;CHECK(owner.Acquire(scope));
        LauncherLinks home(scope);CHECK(home.Open());LaunchLinks links;CHECK(!home.Take(links)&&!links.Any());
        wchar_t exe[32768]{};CHECK(GetModuleFileNameW(nullptr,exe,32768));
        std::wstring command=L"\""+std::wstring(exe)+L"\" "+scope;
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
        CHECK(CreateProcessW(exe,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child));
        CloseHandle(child.hThread);
        const auto wait=WaitForSingleObject(child.hProcess,5000);
        if(wait!=WAIT_OBJECT_0){TerminateProcess(child.hProcess,1);WaitForSingleObject(child.hProcess,5000);}
        DWORD exit=1;CHECK(GetExitCodeProcess(child.hProcess,&exit));CloseHandle(child.hProcess);
        CHECK(wait==WAIT_OBJECT_0&&exit==0);
        CHECK(home.Take(links)&&links.Any());
        CHECK(links.join=="7K3M0X1RT9PZ"&&links.match.matchId==Match&&links.room.roomId==Room&&
            links.connect==Bridge&&links.replay=="private-fixture.sf4r"&&links.discord);
        CHECK(!home.Take(links)); // A Start press cannot consume the same intent twice.
        JoinLinkSlot invalid{};std::memset(invalid.code,'!',sizeof(invalid.code));
        CHECK(DeliverSlot(invalid,name(JoinLinkSectionName()).c_str(),name(JoinLinkEventName()).c_str()));
        CHECK(DeliverSlot(DiscordLaunchSlot{9},name(DiscordLaunchSectionName()).c_str(),name(DiscordLaunchEventName()).c_str()));
        CHECK(!home.Take(links)); // Reject malformed input even from another local process.
    }
    CHECK(!DeliverDiscordLaunch(name(DiscordLaunchSectionName()).c_str(),name(DiscordLaunchEventName()).c_str()));
    CHECK(!DeliverJoinLink("7K3M0X1RT9PZ",name(JoinLinkSectionName()).c_str(),name(JoinLinkEventName()).c_str()));
    // A game receiver can now own a fresh, empty section.
    JoinLinkMailbox game(name(JoinLinkSectionName()).c_str(),name(JoinLinkEventName()).c_str());
    CHECK(game.Open()&&game.Take().empty());
    // Model launcher -> updater -> replacement launcher with real process
    // handles. The updater retains the slots, without consuming their data.
    const auto restartScope=scope+L".restart";
    auto first=std::make_unique<LauncherLinks>(restartScope);CHECK(first->Open());
    CHECK(DeliverJoinLink("7K3M0X1RT9PZ",(std::wstring(JoinLinkSectionName())+restartScope).c_str(),
        (std::wstring(JoinLinkEventName())+restartScope).c_str()));
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,(restartScope+L".release").c_str());CHECK(release);
    wchar_t exe[32768]{};CHECK(GetModuleFileNameW(nullptr,exe,32768));
    std::wstring command=L"\""+std::wstring(exe)+L"\" "+restartScope+L" hold";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION updater{};
    CHECK(CreateProcessW(exe,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&updater));
    CloseHandle(updater.hThread);
    CHECK(LauncherLinks::WaitForReceiver(updater.hProcess,restartScope));
    first.reset();
    LauncherLinks replacement(restartScope);CHECK(replacement.Open());
    CHECK(SetEvent(release));CHECK(WaitForSingleObject(updater.hProcess,5000)==WAIT_OBJECT_0);
    DWORD exit=1;CHECK(GetExitCodeProcess(updater.hProcess,&exit)&&exit==0);
    CloseHandle(updater.hProcess);CloseHandle(release);
    LaunchLinks pending;CHECK(replacement.Take(pending)&&pending.join=="7K3M0X1RT9PZ");
    CHECK(!replacement.Take(pending));
    return 0;
}
