#include "Speech.hxx"
#include "SpeechLinux.hxx"
#include "Utf8.hxx"
#include "../common/Localization.hxx"
#include <sapi.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sf4e { namespace speech {
namespace {
// An empty text with interrupt is Stop.
struct Request { std::string text; bool interrupt; int rate, volume; std::string language; };
std::mutex lock;
std::condition_variable wake;
std::vector<Request> pending;
bool started = false;
std::atomic<Missing> missing{Missing::Nothing};

// Wine's own way to run a Unix program (winebrowser runs xdg-open with it).
// It is internal to Wine, so a Wine without it has no voice for Ember.
using UnixSpawn = LONG (WINAPI*)(char* const*, int);
FARPROC Ntdll(const char* name) { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name); }

// speech-dispatcher through a relay on the host (SpeechLinux.hxx). The FIFO
// lives in the prefix's temporary folder, which the container and the host
// both see under the same Unix path.
void RunLinux(UnixSpawn spawn) {
    using UnixName = char* (WINAPI*)(const wchar_t*);
    const auto unixName = reinterpret_cast<UnixName>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name"));
    wchar_t folder[MAX_PATH] = {}, bus[512] = {};
    GetTempPathW(MAX_PATH, folder);
    GetEnvironmentVariableW(L"DBUS_SESSION_BUS_ADDRESS", bus, 512);
    char* unixFolder = unixName ? unixName(folder) : nullptr;
    const std::string unixFolderName = unixFolder ? unixFolder : "";
    HeapFree(GetProcessHeap(), 0, unixFolder);
    // Unique per run: a relay of an earlier run with the same process id may still be leaving.
    const std::string name = "ember-speech-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const bool container = GetFileAttributesW(L"Z:\\run\\host\\usr\\bin\\busctl") != INVALID_FILE_ATTRIBUTES;
    HANDLE fifo = INVALID_HANDLE_VALUE;
    std::wstring ready;
    const auto relayReady = [&] { return !ready.empty() && GetFileAttributesW(ready.c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (spawn && !unixFolderName.empty() && (*bus || !container)) {
        auto command = RelayCommand(container, platform::WideToUtf8(bus), name + ".service", unixFolderName + "/" + name + ".fifo");
        std::vector<char*> argv;
        for (auto& argument : command) argv.push_back(&argument[0]);
        argv.push_back(nullptr);
        // busctl returns once systemd has the unit; python3 itself is not waited for.
        if (!spawn(argv.data(), container)) {
            const std::wstring path = std::wstring(folder) + platform::Utf8ToWide(name.c_str()) + L".fifo";
            ready = path + L".ready";
            // The relay leaves its ready file once speech-dispatcher answers
            // and it reads the FIFO. None in ten seconds: no speech.
            for (int tries = 0; tries < 200 && !relayReady(); ++tries) Sleep(50);
            if (relayReady()) fifo = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
    }
    spdlog::info("Speech: speech-dispatcher through a relay {}; {}", container ? "started by the host's systemd" : "started directly",
        fifo == INVALID_HANDLE_VALUE ? "the relay did not answer" : "connected");
    // A relay that left takes its ready file with it; Wine's writes alone would not tell.
    const auto write = [&](const std::string& message) {
        DWORD written = 0;
        if (fifo == INVALID_HANDLE_VALUE || !relayReady() || !WriteFile(fifo, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) {
            if (missing == Missing::Nothing) spdlog::warn("Speech: the speech relay is gone");
            missing = Missing::SpeechDispatcher;
        }
    };
    write("SET SELF CLIENT_NAME user:ember:menus\r\n");
    for (;;) {
        std::vector<Request> next;
        {
            std::unique_lock<std::mutex> held(lock);
            wake.wait(held, [] { return !pending.empty(); });
            next.swap(pending);
        }
        if (missing != Missing::Nothing) continue;
        for (const auto& request : next) write(SsipMessage(request.text, request.interrupt, request.rate, request.volume, request.language));
    }
}

// The voice is made and used on this thread only. Creating it takes long
// enough to drop a frame, and the game's threads keep their COM apartments.
// The thread lives as long as the process, like the DLL it runs in.
void Run() {
    if (Ntdll("wine_get_version")) return RunLinux(reinterpret_cast<UnixSpawn>(Ntdll("__wine_unix_spawnvp")));
    ISpVoice* voice = nullptr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ||
        FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice)))) {
        spdlog::warn("Speech: SAPI could not be started");
        missing = Missing::WindowsVoice;
    }
    for (;;) {
        std::vector<Request> next;
        {
            std::unique_lock<std::mutex> held(lock);
            wake.wait(held, [] { return !pending.empty(); });
            next.swap(pending);
        }
        if (missing != Missing::Nothing) continue;
        for (const auto& request : next) {
            const DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML | (request.interrupt ? SPF_PURGEBEFORESPEAK : 0);
            if (request.text.empty()) { voice->Speak(nullptr, flags, nullptr); continue; }
            voice->SetRate(request.rate);
            voice->SetVolume(static_cast<USHORT>(request.volume));
            // A Windows without a voice installed fails here (SPERR_NOT_FOUND).
            const HRESULT spoken = voice->Speak(platform::Utf8ToWide(request.text.c_str()).c_str(), flags, nullptr);
            if (FAILED(spoken)) {
                spdlog::warn("Speech: SAPI has no voice to speak with ({:08x})", static_cast<unsigned long>(spoken));
                missing = Missing::WindowsVoice;
            }
        }
    }
}
void Queue(Request request) {
    {
        std::lock_guard<std::mutex> held(lock);
        // An interruption makes everything still waiting moot.
        if (request.interrupt) pending.clear();
        pending.push_back(std::move(request));
        if (!started) { started = true; std::thread(Run).detach(); }
    }
    wake.notify_one();
}
}

void Say(const std::string& text, bool interrupt, int rate, int volume) {
    if (!text.empty()) Queue({text, interrupt, rate, volume, loc::Tag(loc::Active())});
}
void Stop() { Queue({{}, true, 0, 0, {}}); }
Missing Unavailable() { return missing; }
} }
