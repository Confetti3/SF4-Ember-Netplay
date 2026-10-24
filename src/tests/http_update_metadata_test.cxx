// Real WinHTTP requests against loopback: cancellation, one overall deadline,
// bounded responses, HTTP failures and truncated bodies. No GitHub dependency.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../common/sf4e__NetUtil.hxx"
#include "test_support.hxx"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
using Clock=std::chrono::steady_clock;
class Server {
public:
    explicit Server(std::string reply, bool hold=false) : reply_(std::move(reply)),hold_(hold) {
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(listener_!=INVALID_SOCKET && bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
        int length=sizeof(address);
        CHECK(getsockname(listener_,reinterpret_cast<sockaddr*>(&address),&length)==0 && listen(listener_,4)==0);
        port_=ntohs(address.sin_port); thread_=std::thread([this]{Serve();});
    }
    ~Server() { closesocket(listener_); thread_.join(); }
    int Port() const { return port_; }
    std::atomic<bool> received{false};
private:
    void Serve() {
        for(;;) {
            auto client=accept(listener_,nullptr,nullptr); if(client==INVALID_SOCKET)return;
            DWORD timeout=10000;
            setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            std::string request; char buffer[4096]; int count;
            while(request.find("\r\n\r\n")==std::string::npos && (count=recv(client,buffer,sizeof(buffer),0))>0) request.append(buffer,count);
            received=true;
            for(size_t sent=0;sent<reply_.size();) {
                count=send(client,reply_.data()+sent,static_cast<int>(reply_.size()-sent),0);
                if(count<=0)break; sent+=count;
            }
            if(hold_)while(recv(client,buffer,sizeof(buffer),0)>0){}
            closesocket(client);
        }
    }
    SOCKET listener_=INVALID_SOCKET; int port_=0; std::string reply_; bool hold_; std::thread thread_;
};
bool Get(Server& server,std::string& body,int budget=3000,const std::atomic<bool>* cancel=nullptr) {
    return sf4e::HttpGetUpdateMetadata("127.0.0.1",server.Port(),false,"/release",budget,
        [cancel]{return cancel && cancel->load();},body);
}
std::string Reply(int status,const std::string& body) {
    return "HTTP/1.1 "+std::to_string(status)+" Result\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body;
}
int main() {
    WSADATA data; CHECK(WSAStartup(MAKEWORD(2,2),&data)==0);
    {
        std::string body="stale";
        Server success(Reply(200,"{\"tag_name\":\"v1.4.0\"}")); CHECK(Get(success,body) && body=="{\"tag_name\":\"v1.4.0\"}");
        Server chunked("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2\r\n{}\r\n0\r\n\r\n");
        CHECK(Get(chunked,body) && body=="{}");
        Server failure(Reply(403,"rate limited")); CHECK(!Get(failure,body) && body.empty());
        Server empty(Reply(200,"")); CHECK(!Get(empty,body) && body.empty());
        Server huge(Reply(200,std::string(1024*1024+1,'x'))); CHECK(!Get(huge,body) && body.empty());
        Server truncated("HTTP/1.1 200 OK\r\nContent-Length: 500\r\nConnection: close\r\n\r\npartial");
        CHECK(!Get(truncated,body) && body.empty());
        Server redirected("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/untrusted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        CHECK(!Get(redirected,body) && body.empty());
        Server early("",true); std::atomic<bool> cancelled{true};
        CHECK(!Get(early,body,3000,&cancelled) && body.empty() && !early.received);
    }
    // Cancel both a pending header receive and an in-progress body read.
    for(const std::string reply: {std::string(),std::string("HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\npartial")}) {
        Server silent(reply,true); std::atomic<bool> cancel{false},done{false}; bool ok=true; std::string body;
        std::thread reader([&]{ok=Get(silent,body,10000,&cancel);done=true;});
        const auto deadline=Clock::now()+std::chrono::seconds(5);
        while(!silent.received && Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(silent.received);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); CHECK(!done);
        const auto start=Clock::now(); cancel=true; reader.join();
        CHECK(Clock::now()-start<std::chrono::seconds(1) && !ok && body.empty());
    }
    {
        Server silent("",true); std::string body; const auto start=Clock::now();
        CHECK(!Get(silent,body,200) && body.empty());
        CHECK(Clock::now()-start<std::chrono::seconds(2));
    }
    WSACleanup(); std::cout<<"Update metadata HTTP tests passed\n";
}
