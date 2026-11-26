#include "sf4e__QuickLines.hxx"

#include <atomic>
#include <deque>
#include <mutex>
#include <windows.h>

#include "../common/QuickChat.hxx"

namespace sf4e { namespace overlay {
namespace {
struct Posted { int slot; unsigned long long atMs; };
std::mutex lock;
std::deque<Posted> pending;
std::atomic<bool> live{false};
}
void SetQuickLinesLive(bool now) { live = now; }
bool QuickLinesLive() { return live; }
void PostQuickLine(int slot) {
    std::lock_guard<std::mutex> guard(lock);
    pending.push_back({slot, GetTickCount64()});
    if (pending.size() > static_cast<std::size_t>(quickchat::Slots)) pending.pop_front();
}
std::vector<int> TakeQuickLines() {
    std::lock_guard<std::mutex> guard(lock);
    const unsigned long long now = GetTickCount64();
    std::vector<int> taken;
    for (const auto& posted : pending) if (now - posted.atMs <= StaleMs) taken.push_back(posted.slot);
    pending.clear();
    return taken;
}
} }
