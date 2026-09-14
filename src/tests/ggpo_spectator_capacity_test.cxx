#include <winsock2.h>
#include <ggponet.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#define CHECK(c) do { if(!(c)) { std::cerr<<"GGPO capacity failure at "<<__LINE__<<'\n';return 1;} } while(false)
int main() {
    WSADATA winsock{};CHECK(WSAStartup(MAKEWORD(2,2),&winsock)==0);
    const SOCKET sink=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);CHECK(sink!=INVALID_SOCKET);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(sink,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    int length=sizeof(address);CHECK(getsockname(sink,reinterpret_cast<sockaddr*>(&address),&length)==0);
    GGPOSessionCallbacks callbacks{};
    callbacks.begin_game=[](const char*){return true;};
    callbacks.save_game_state=[](unsigned char** data,int* size,int* checksum,int){*data=static_cast<unsigned char*>(std::calloc(1,1));*size=1;*checksum=0;return *data!=nullptr;};
    callbacks.load_game_state=[](unsigned char*,int){return true;};
    callbacks.log_game_state=[](char*,unsigned char*,int){return true;};
    callbacks.free_buffer=[](void* data){std::free(data);};
    callbacks.advance_frame=[](int){return true;};
    callbacks.on_event=[](GGPOEvent*){return true;};
    GGPOSession* session=nullptr;
    CHECK(ggpo_start_session(&session,&callbacks,"capacity-test",GGPO_MAX_PLAYERS,1,0)==GGPO_OK);
    for(int index=0;index<GGPO_MAX_PLAYERS+GGPO_MAX_SPECTATORS+1;++index) {
        GGPOPlayer player{};player.size=sizeof(player);player.player_num=index+1;
        player.type=index==0?GGPO_PLAYERTYPE_LOCAL:index<GGPO_MAX_PLAYERS?GGPO_PLAYERTYPE_REMOTE:GGPO_PLAYERTYPE_SPECTATOR;
        if(index) { strcpy_s(player.u.remote.ip_address,"127.0.0.1");player.u.remote.port=ntohs(address.sin_port); }
        GGPOPlayerHandle handle;
        const auto result=ggpo_add_player(session,&player,&handle);
        CHECK(result==(index==GGPO_MAX_PLAYERS+GGPO_MAX_SPECTATORS?GGPO_ERRORCODE_TOO_MANY_SPECTATORS:GGPO_OK));
    }
    CHECK(ggpo_close_session(session)==GGPO_OK);
    closesocket(sink);WSACleanup();
    std::cout<<"GGPO spectator capacity passed: full player and spectator limits, overflow rejected.\n";
}
