#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace sf4e { namespace platform {
// The SDK's registered command contains no ticket. Repeated launches simply
// leave the existing companion in charge of receiving the Discord callback.
class LauncherInstance {
public:
    ~LauncherInstance() { if (mutex_) CloseHandle(mutex_); }
    // Tests use a per-run scope to exercise real named-mutex contention without
    // competing with the user's launcher. Production callers keep the default.
    bool Acquire(const std::wstring& scope = L"") {
        HANDLE token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
        DWORD size=0; GetTokenInformation(token,TokenUser,nullptr,0,&size);
        std::vector<unsigned char> bytes(size);
        const bool ok=GetTokenInformation(token,TokenUser,bytes.data(),size,&size)!=FALSE;
        CloseHandle(token); if(!ok) return false;
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)) return false;
        const std::wstring name=L"Local\\Ember.Launcher."+std::wstring(sid)+scope;
        const std::wstring sddl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)) return false;
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        mutex_=CreateMutexW(&security,FALSE,name.c_str());
        const auto error=GetLastError(); LocalFree(descriptor);
        return mutex_ && error!=ERROR_ALREADY_EXISTS;
    }
private:
    HANDLE mutex_=nullptr;
};
} }
