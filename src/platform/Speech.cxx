#include "Speech.hxx"
#include "Utf8.hxx"
#include <sapi.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sf4e { namespace speech {
namespace {
// An empty text with interrupt is Stop.
struct Request { std::wstring text; bool interrupt; int rate, volume; };
std::mutex lock;
std::condition_variable wake;
std::vector<Request> pending;
bool started = false;

// The voice is made and used on this thread only. Creating it takes long
// enough to drop a frame, and the game's threads keep their COM apartments.
// The thread lives as long as the process, like the DLL it runs in.
void Run() {
    ISpVoice* voice = nullptr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ||
        FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice)))) voice = nullptr;
    for (;;) {
        std::vector<Request> next;
        {
            std::unique_lock<std::mutex> held(lock);
            wake.wait(held, [] { return !pending.empty(); });
            next.swap(pending);
        }
        if (!voice) continue;
        for (const auto& request : next) {
            const DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML | (request.interrupt ? SPF_PURGEBEFORESPEAK : 0);
            if (request.text.empty()) { voice->Speak(nullptr, flags, nullptr); continue; }
            voice->SetRate(request.rate);
            voice->SetVolume(static_cast<USHORT>(request.volume));
            voice->Speak(request.text.c_str(), flags, nullptr);
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
    auto wide = platform::Utf8ToWide(text.c_str());
    if (!wide.empty()) Queue({std::move(wide), interrupt, rate, volume});
}
void Stop() { Queue({{}, true, 0, 0}); }
} }
