#include "../platform/LauncherHomeLinks.hxx"
#include "../ui/LauncherScreen.hxx"
#include "test_support.hxx"
using namespace sf4e::platform;
static const char* Bridge="brg_0dbc0598-2312-4ce3-9df8-e160330565e6";
static const char* Match="emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
static const char* Room="0123456789abcdef0123456789abcdef";
int wmain(int argc,wchar_t** argv) {
    const std::wstring scope=argc==2?argv[1]:L".HomeTest."+std::to_wstring(GetCurrentProcessId());
    const auto name=[&](const wchar_t* base){return std::wstring(base)+scope;};
    if(argc==2){
        CHECK(DeliverJoinLink("7K3M0X1RT9PZ",name(JoinLinkSectionName()).c_str(),name(JoinLinkEventName()).c_str()));
        CHECK(DeliverMatchLink(sf4e::tournament_link::Checked(Bridge,Match),name(MatchLinkSectionName()).c_str(),name(MatchLinkEventName()).c_str()));
        CHECK(DeliverPublicRoomLink(sf4e::tournament_link::CheckedRoom(Bridge,Room),name(PublicRoomLinkSectionName()).c_str(),name(PublicRoomLinkEventName()).c_str()));
        CHECK(DeliverConnectLink(Bridge,name(ConnectLinkSectionName()).c_str(),name(ConnectLinkEventName()).c_str()));
        CHECK(DeliverReplayLink("private-fixture.sf4r",name(ReplayLinkSectionName()).c_str(),name(ReplayLinkEventName()).c_str()));
        CHECK(DeliverDiscordLaunch(name(DiscordLaunchSectionName()).c_str(),name(DiscordLaunchEventName()).c_str()));
        return 0;
    }
    {
        LauncherHomeLinks home(scope);CHECK(home.Open());HomeLaunchLinks links;CHECK(!home.Take(links)&&!links.Any());
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
    using namespace sf4e::ui;
    CHECK(StartWithoutSurface(LauncherRoot::Home));
    CHECK(!StartWithoutSurface(LauncherRoot::Recovery)&&!StartWithoutSurface(LauncherRoot::Updates));
    return 0;
}
