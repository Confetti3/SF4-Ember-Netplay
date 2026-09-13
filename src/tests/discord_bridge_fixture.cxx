#include "../discord/BridgeServer.hxx"
int wmain(int argc,wchar_t** argv) {
    if(argc!=3 || std::wstring(argv[1])!=L"--pipe") return 1;
    sf4e::discord::BridgeServer server;
    if(!server.Open(argv[2])) return 2;
    while(server.Alive()) {
        std::string message; bool present=false;
        if(!server.Receive(message,present)) return 0;
        if(present && (message=="shutdown" || !server.Send(message))) return 0;
        Sleep(2);
    }
    return 0;
}
