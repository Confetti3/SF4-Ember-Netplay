#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace sf4e { namespace platform {

// POD copied into the game bootstrap payload. The nonce is a per-run secret;
// never serialize this structure into settings, command lines, or diagnostics.
struct HelperBootstrap {
    uint32_t version = 1;
    uint32_t helperPid = 0;
    wchar_t pipeName[96] = {};
    uint8_t nonce[32] = {};
};

// Launcher owns this for the full game lifetime, including when its temporary
// UI closes. A Windows job ensures no orphan helper survives launcher exit.
class HelperProcess {
public:
    HelperProcess() = default;
    ~HelperProcess();
    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;

    bool Start(const std::wstring& executable, DWORD gamePid, bool relayOnly = false);
    bool IsRunning() const;
    void Stop(DWORD graceMs = 2000);
    const HelperBootstrap& Bootstrap() const { return bootstrap_; }
    DWORD LastError() const { return error_; }

private:
    HelperBootstrap bootstrap_;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    DWORD error_ = ERROR_SUCCESS;
};

} }
