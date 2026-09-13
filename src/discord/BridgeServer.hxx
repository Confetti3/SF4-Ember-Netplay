#pragma once
#include <windows.h>
#include <sddl.h>
#include <array>
#include <vector>
#include <string>
#include <cstdint>

namespace sf4e { namespace discord {
// Server half of the existing HelperClient framing. Only the injected game
// process can authenticate. All waits run in the companion, never the game.
class BridgeServer {
public:
    ~BridgeServer() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        if (game_) CloseHandle(game_);
    }
    bool Open(const std::wstring& name) {
        const std::wstring prefix = L"\\\\.\\pipe\\sf4-net-";
        if (name.size() != prefix.size()+32 || name.compare(0,prefix.size(),prefix)) return false;
        for (size_t i=prefix.size(); i<name.size(); ++i)
            if ((name[i]<L'0'||name[i]>L'9') && (name[i]<L'a'||name[i]>L'f')) return false;
        std::array<unsigned char,42> bootstrap{};
        DWORD got=0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE),bootstrap.data(),42,&got,nullptr) || got!=42 ||
            memcmp(bootstrap.data(),"SF4N\0\1",6)) return false;
        DWORD pid=0; for(int i=6;i<10;++i) pid=(pid<<8)|bootstrap[i];
        game_=OpenProcess(SYNCHRONIZE,FALSE,pid);
        if (!game_) return false;
        HANDLE token=nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
        DWORD size=0; GetTokenInformation(token,TokenUser,nullptr,0,&size);
        std::vector<unsigned char> user(size);
        const bool ok=GetTokenInformation(token,TokenUser,user.data(),size,&size)!=FALSE;
        CloseHandle(token); if(!ok) return false;
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid)) return false;
        const std::wstring sddl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")"; LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)) return false;
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        pipe_=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security);
        LocalFree(descriptor);
        if(pipe_==INVALID_HANDLE_VALUE) return false;
        OVERLAPPED connect{}; connect.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!connect.hEvent) return false;
        bool connected=ConnectNamedPipe(pipe_,&connect)!=FALSE;
        if(!connected) {
            const DWORD error=GetLastError();
            if(error==ERROR_PIPE_CONNECTED) connected=true;
            else if(error==ERROR_IO_PENDING) {
                HANDLE waits[]={game_,connect.hEvent};
                connected=WaitForMultipleObjects(2,waits,FALSE,60000)==WAIT_OBJECT_0+1;
                if(!connected) CancelIoEx(pipe_,&connect);
                DWORD unused=0;
                connected=GetOverlappedResult(pipe_,&connect,&unused,TRUE) && connected;
            }
        }
        CloseHandle(connect.hEvent);
        ULONG client=0;
        if(!connected || !GetNamedPipeClientProcessId(pipe_,&client) || client!=pid) return false;
        std::uint64_t id=0; std::string auth;
        bool valid=Read(id,auth) && id==1 && auth.size()==32;
        unsigned difference=0;
        if(valid) for(size_t i=0;i<32;++i) difference|=static_cast<unsigned char>(auth[i])^bootstrap[10+i];
        valid=valid && difference==0;
        SecureZeroMemory(bootstrap.data(),bootstrap.size());
        if(!auth.empty()) SecureZeroMemory(&auth[0],auth.size());
        if(!valid || !Write(1,"SF4N")) return false;
        lastRead_=1; nextWrite_=2; return true;
    }
    bool Alive() const { return game_ && WaitForSingleObject(game_,0)==WAIT_TIMEOUT; }
    bool Receive(std::string& payload, bool& present) {
        present=false; DWORD available=0;
        if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr)) return false;
        if(!available) return true;
        std::uint64_t id=0;
        if(!Read(id,payload) || id<=lastRead_) return false;
        lastRead_=id; present=true; return true;
    }
    bool Send(const std::string& payload) { return Write(nextWrite_++,payload); }
private:
    bool Transfer(void* buffer,DWORD count,bool write) {
        auto* bytes=static_cast<unsigned char*>(buffer); DWORD offset=0;
        const auto deadline=GetTickCount64()+1000;
        while(offset<count) {
            OVERLAPPED operation{}; operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!operation.hEvent) return false;
            DWORD transferred=0;
            bool ok=(write ? WriteFile(pipe_,bytes+offset,count-offset,&transferred,&operation) :
                ReadFile(pipe_,bytes+offset,count-offset,&transferred,&operation))!=FALSE;
            if(!ok && GetLastError()==ERROR_IO_PENDING) {
                HANDLE waits[]={game_,operation.hEvent};
                const auto now=GetTickCount64();
                ok=WaitForMultipleObjects(2,waits,FALSE,now<deadline?static_cast<DWORD>(deadline-now):0)==WAIT_OBJECT_0+1;
                if(!ok) CancelIoEx(pipe_,&operation);
                ok=GetOverlappedResult(pipe_,&operation,&transferred,TRUE) && ok;
            }
            CloseHandle(operation.hEvent);
            if(!ok || !transferred) return false;
            offset+=transferred;
        }
        return true;
    }
    bool Read(std::uint64_t& id,std::string& payload) {
        unsigned char header[14]{};
        if(!Transfer(header,14,false)) return false;
        std::uint32_t length=0; for(int i=0;i<4;++i) length=(length<<8)|header[i];
        if(length<=10 || length>4106 || header[4] || header[5]!=1) return false;
        id=0; for(int i=6;i<14;++i) id=(id<<8)|header[i];
        if(!id || id>INT64_MAX) return false;
        payload.resize(length-10); return Transfer(&payload[0],length-10,false);
    }
    bool Write(std::uint64_t id,const std::string& payload) {
        if(payload.empty() || payload.size()>4096 || id>INT64_MAX) return false;
        std::vector<unsigned char> bytes(14+payload.size());
        const auto length=static_cast<std::uint32_t>(payload.size()+10);
        for(int i=0;i<4;++i) bytes[i]=static_cast<unsigned char>(length>>(24-i*8));
        bytes[5]=1;
        for(int i=0;i<8;++i) bytes[6+i]=static_cast<unsigned char>(id>>(56-i*8));
        memcpy(bytes.data()+14,payload.data(),payload.size());
        return Transfer(bytes.data(),static_cast<DWORD>(bytes.size()),true);
    }
    HANDLE pipe_=INVALID_HANDLE_VALUE,game_=nullptr;
    std::uint64_t lastRead_=0,nextWrite_=1;
};
} }
