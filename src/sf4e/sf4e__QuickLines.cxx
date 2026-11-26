#include "sf4e__QuickLines.hxx"

#include <atomic>

namespace sf4e { namespace overlay {
namespace {
std::atomic<unsigned> pending{0};
std::atomic<bool> live{false};
}
void SetQuickLinesLive(bool now) { live = now; }
bool QuickLinesLive() { return live; }
void PostQuickLine(int slot) { pending.fetch_or(1u << slot); }
unsigned TakeQuickLines() { return pending.exchange(0); }
} }
