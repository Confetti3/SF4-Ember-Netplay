#include "../platform/HelperClient.hxx"
#include "../platform/LauncherInstance.hxx"
#include <iostream>
#include "test_support.hxx"
int wmain(int argc,wchar_t** argv) {
    using namespace sf4e::platform;
    CHECK(argc==2);
    const auto scope=L".Test."+std::to_wstring(GetCurrentProcessId());
    {
        LauncherInstance first,second;
        CHECK(first.Acquire(scope)); CHECK(!second.Acquire(scope));
    }
    {
        LauncherInstance reopened;
        CHECK(reopened.Acquire(scope));
    }
    for(bool badNonce : {false,true}) {
        HelperProcess server;
        CHECK(server.Start(argv[1],GetCurrentProcessId()));
        auto bootstrap=server.Bootstrap();
        if(badNonce) bootstrap.nonce[0]^=1;
        HelperClient client; CHECK(client.Start(bootstrap));
        const auto deadline=GetTickCount64()+5000;
        while(client.State()==HelperState::Connecting && GetTickCount64()<deadline) Sleep(2);
        if(badNonce) { CHECK(client.State()==HelperState::Failed); continue; }
        CHECK(client.State()==HelperState::Connected);
        CHECK(client.Send("private-fixture-message"));
        HelperMessage reply; bool received=false;
        while(!(received=client.TryReceive(reply)) && GetTickCount64()<deadline) Sleep(2);
        CHECK(received && reply.payload=="private-fixture-message");
        // Companion failure must only fail its own local client.
        server.Stop(0);
        while(client.State()==HelperState::Connected && GetTickCount64()<deadline) Sleep(2);
        CHECK(client.State()==HelperState::Failed);
    }
    return 0;
}
