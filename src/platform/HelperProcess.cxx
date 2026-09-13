#include "HelperProcess.hxx"

#include <bcrypt.h>
#include <vector>

namespace sf4e { namespace platform {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};
struct Attributes {
    std::vector<ULONG_PTR> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    ~Attributes() { if (list) { DeleteProcThreadAttributeList(list); } }
    bool Init(HANDLE* handles, size_t count) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize((size + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
        auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size)) { return false; }
        list = candidate;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
            sizeof(HANDLE) * count, nullptr, nullptr) != FALSE;
    }
};
}

HelperProcess::~HelperProcess() { Stop(0); }

bool HelperProcess::Start(const std::wstring& executable, DWORD gamePid, bool relayOnly) {
    if (process_ || gamePid == 0 || executable.empty() || executable.find(L'"') != std::wstring::npos) {
        error_ = ERROR_INVALID_PARAMETER; return false;
    }
    DWORD binaryType = 0;
    if (!GetBinaryTypeW(executable.c_str(), &binaryType)) { error_ = GetLastError(); return false; }
    if (binaryType != SCS_64BIT_BINARY) { error_ = ERROR_BAD_EXE_FORMAT; return false; }
    error_ = ERROR_SUCCESS;
    bootstrap_ = HelperBootstrap();
    uint8_t publicName[16];
    if (BCryptGenRandom(nullptr, publicName, sizeof(publicName), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0
        || BCryptGenRandom(nullptr, bootstrap_.nonce, sizeof(bootstrap_.nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        error_ = ERROR_GEN_FAILURE; return false;
    }
    std::wstring name = L"\\\\.\\pipe\\sf4-net-";
    const wchar_t* hex = L"0123456789abcdef";
    for (uint8_t byte : publicName) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    wcscpy_s(bootstrap_.pipeName, name.c_str());

    SECURITY_ATTRIBUTES security = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    Handle read, write, nullOutput;
    if (!CreatePipe(&read.value, &write.value, &security, 0)
        || !SetHandleInformation(write.value, HANDLE_FLAG_INHERIT, 0)) { error_ = GetLastError(); return false; }
    nullOutput.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullOutput.value == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
    HANDLE inherited[] = { read.value, nullOutput.value };
    Attributes attributes;
    if (!attributes.Init(inherited, 2)) { error_ = GetLastError(); return false; }

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) { error_ = GetLastError(); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error_ = GetLastError(); Stop(0); return false;
    }
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = read.value;
    startup.StartupInfo.hStdOutput = nullOutput.value;
    startup.StartupInfo.hStdError = nullOutput.value;
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION child = {};
    std::wstring command = L"\"" + executable + L"\" --pipe \"" + name + L"\"";
    if (relayOnly) command += L" --relay-only";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &child)) {
        error_ = GetLastError(); Stop(0); return false;
    }
    process_ = child.hProcess;
    Handle thread; thread.value = child.hThread;
    if (!AssignProcessToJobObject(job_, process_)) {
        error_ = GetLastError(); TerminateProcess(process_, 1); Stop(0); return false;
    }
    bootstrap_.helperPid = child.dwProcessId;
    uint8_t bytes[42] = { 'S', 'F', '4', 'N', 0, 1 };
    for (int index = 0; index < 4; ++index) { bytes[6 + index] = static_cast<uint8_t>(gamePid >> (24 - 8 * index)); }
    memcpy(bytes + 10, bootstrap_.nonce, sizeof(bootstrap_.nonce));
    DWORD written = 0;
    const bool delivered = WriteFile(write.value, bytes, sizeof(bytes), &written, nullptr) && written == sizeof(bytes);
    SecureZeroMemory(bytes, sizeof(bytes));
    if (!delivered || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        error_ = GetLastError(); Stop(0); return false;
    }
    return true;
}

bool HelperProcess::IsRunning() const {
    DWORD status = 0;
    return process_ && GetExitCodeProcess(process_, &status) && status == STILL_ACTIVE;
}

void HelperProcess::Stop(DWORD graceMs) {
    if (process_) {
        if (WaitForSingleObject(process_, graceMs) != WAIT_OBJECT_0 && job_) {
            TerminateJobObject(job_, 1);
            WaitForSingleObject(process_, 1000);
        }
        CloseHandle(process_); process_ = nullptr;
    }
    if (job_) { CloseHandle(job_); job_ = nullptr; }
    SecureZeroMemory(&bootstrap_, sizeof(bootstrap_));
}

} }
