#include "Speech.hxx"
#include "SpeechLinux.hxx"
#include "Utf8.hxx"
#include "../common/Localization.hxx"
#include <sapi.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sf4e { namespace speech {
namespace {
// No parts with interrupt is Stop; render asks only for clips made ahead.
struct Request { std::vector<std::string> parts; bool interrupt, render; int rate, volume; std::string language; };
std::mutex lock;
std::condition_variable wake;
std::vector<Request> pending;
bool started = false;
std::atomic<Missing> missing{Missing::Nothing};
std::atomic<Voice> voice{Voice::System};
std::atomic<int> percent{0};

std::vector<Request> Next(std::chrono::milliseconds wait) {
    std::vector<Request> next;
    std::unique_lock<std::mutex> held(lock);
    wake.wait_for(held, wait, [] { return !pending.empty(); });
    next.swap(pending);
    return next;
}

// Wine's own way to run a Unix program (winebrowser runs xdg-open with it).
// It is internal to Wine, so a Wine without it has no voice for Ember.
using UnixSpawn = LONG (WINAPI*)(char* const*, int);
FARPROC Ntdll(const char* name) { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name); }
using UnixName = char* (WINAPI*)(const wchar_t*);
std::string UnixPath(const std::wstring& path) {
    const auto unixName = reinterpret_cast<UnixName>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name"));
    char* name = unixName ? unixName(path.c_str()) : nullptr;
    const std::string result = name ? name : "";
    HeapFree(GetProcessHeap(), 0, name);
    return result;
}
// This DLL's folder, which holds speech_relay.py.
std::wstring ModuleFolder() {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Next), &module);
    const std::wstring file(path, GetModuleFileNameW(module, path, MAX_PATH));
    return file.substr(0, file.find_last_of(L'\\') + 1);
}

// The relay on the host (SpeechLinux.hxx). Its FIFO lives in the prefix's
// temporary folder, which the container and the host both see under the same
// Unix path.
void RunLinux(UnixSpawn spawn) {
    wchar_t folder[MAX_PATH] = {}, bus[512] = {};
    GetTempPathW(MAX_PATH, folder);
    GetEnvironmentVariableW(L"DBUS_SESSION_BUS_ADDRESS", bus, 512);
    const std::string unixFolder = UnixPath(folder), script = UnixPath(ModuleFolder() + L"speech_relay.py");
    // Unique per run: a relay of an earlier run with the same process id may still be leaving.
    const std::string name = "ember-speech-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const bool container = GetFileAttributesW(L"Z:\\run\\host\\usr\\bin\\busctl") != INVALID_FILE_ATTRIBUTES;
    const std::wstring path = std::wstring(folder) + platform::Utf8ToWide(name.c_str()) + L".fifo";
    const std::wstring ready = path + L".ready", status = path + L".status";
    const auto relayReady = [&] { return GetFileAttributesW(ready.c_str()) != INVALID_FILE_ATTRIBUTES; };
    HANDLE fifo = INVALID_HANDLE_VALUE;
    if (spawn && !unixFolder.empty() && !script.empty() && (*bus || !container)) {
        auto command = RelayCommand(container, platform::WideToUtf8(bus), name + ".service", script, unixFolder + "/" + name + ".fifo");
        std::vector<char*> argv;
        for (auto& argument : command) argv.push_back(&argument[0]);
        argv.push_back(nullptr);
        // busctl returns once systemd has the unit; python3 itself is not waited for.
        if (!spawn(argv.data(), container)) {
            // The relay leaves its ready file once speech-dispatcher answers
            // and it reads the FIFO. None in ten seconds: no speech.
            for (int tries = 0; tries < 200 && !relayReady(); ++tries) Sleep(50);
            if (relayReady()) fifo = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
    }
    spdlog::info("Speech: relay {}; {}", container ? "started by the host's systemd" : "started directly",
        fifo == INVALID_HANDLE_VALUE ? "it did not answer" : "connected");
    // A relay that left takes its ready file with it; Wine's writes alone would not tell.
    const auto write = [&](const std::string& line) {
        DWORD written = 0;
        if (fifo == INVALID_HANDLE_VALUE || !relayReady() || !WriteFile(fifo, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
            if (missing == Missing::Nothing) spdlog::warn("Speech: the speech relay is gone");
            missing = Missing::SpeechDispatcher;
        }
    };
    for (;;) {
        // Once a second at least, for the relay's status while it downloads.
        for (const auto& request : Next(std::chrono::seconds(1))) {
            if (missing != Missing::Nothing) break;
            write(request.render ? RenderLine(request.parts, request.rate, request.language) :
                request.parts.empty() ? std::string("STOP\n") : SayLine(request.parts, request.interrupt, request.rate, request.volume, request.language));
        }
        const HANDLE file = CreateFileW(status.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        char text[64] = {};
        DWORD read = 0;
        if (file != INVALID_HANDLE_VALUE) { ReadFile(file, text, sizeof(text) - 1, &read, nullptr); CloseHandle(file); }
        const auto now = ParseRelayStatus(std::string(text, read));
        if (now.voice != voice) spdlog::info("Speech: relay voice {}", text);
        voice = now.voice; percent = now.percent;
    }
}

// The voice is made and used on this thread only. Creating it takes long
// enough to drop a frame, and the game's threads keep their COM apartments.
// The thread lives as long as the process, like the DLL it runs in.
void Run() {
    if (Ntdll("wine_get_version")) return RunLinux(reinterpret_cast<UnixSpawn>(Ntdll("__wine_unix_spawnvp")));
    ISpVoice* sapi = nullptr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ||
        FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sapi)))) {
        spdlog::warn("Speech: SAPI could not be started");
        missing = Missing::WindowsVoice;
    }
    for (;;) {
        for (const auto& request : Next(std::chrono::hours(1))) {
            if (missing != Missing::Nothing || request.render) continue;
            const DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML | (request.interrupt ? SPF_PURGEBEFORESPEAK : 0);
            if (request.parts.empty()) { sapi->Speak(nullptr, flags, nullptr); continue; }
            sapi->SetRate(request.rate);
            sapi->SetVolume(static_cast<USHORT>(request.volume));
            // A Windows without a voice installed fails here (SPERR_NOT_FOUND).
            const HRESULT spoken = sapi->Speak(platform::Utf8ToWide(Line(request.parts).c_str()).c_str(), flags, nullptr);
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

void Say(const std::vector<std::string>& parts, bool interrupt, int rate, int volume) {
    if (!parts.empty()) Queue({parts, interrupt, false, rate, volume, loc::Tag(loc::Active())});
}
void Render(const std::vector<std::string>& parts, int rate) {
    if (!parts.empty()) Queue({parts, false, true, rate, 0, loc::Tag(loc::Active())});
}
void Stop() { Queue({{}, true, false, 0, 0, {}}); }
Missing Unavailable() { return missing; }
VoiceStatus NaturalVoice() { return {voice, percent}; }
} }
