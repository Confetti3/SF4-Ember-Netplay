// The overlay's context is replaced on the game thread while the render thread
// draws it and window messages read it (OverlayLifecycle.hxx). These run the
// three roles on three threads against a heap object standing in for the
// context, freed and reallocated on every change, and fail if any reader ever
// sees it freed or half built.
#include "../ui/OverlayLifecycle.hxx"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using sf4e::ui::OverlayLifecycle;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;
static void Check(bool value, const char* message) {
    if (!value) { ++g_failures; std::printf("FAIL: %s\n", message); }
}

namespace {
constexpr unsigned kAlive = 0x0A11CE55u, kDead = 0xDEADDEADu;
// Stands in for the ImGui context: a header plus a body filled while it is built.
struct Context {
    unsigned magic = kAlive;
    unsigned char body[4096];
    Context() { std::memset(body, 0x5A, sizeof(body)); }
    ~Context() { magic = kDead; std::memset(body, 0, sizeof(body)); }
};

bool Whole(const Context* context) {
    if (!context || context->magic != kAlive) return false;
    for (unsigned char byte : context->body) if (byte != 0x5A) return false;
    return true;
}
}

// Reset on one thread, frames on another and messages on a third.
static void ThreeThreadsNeverSeeAFreedContext() {
    OverlayLifecycle lifecycle;
    Context* context = new Context;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> torn{0}, frames{0}, skipped{0}, messages{0}, changes{0};

    std::thread game([&] {
        while (!stop) {
            {
                OverlayLifecycle::Change change(lifecycle);
                delete context;
                context = nullptr;
                std::this_thread::yield();
                context = new Context;
                ++changes;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::thread render([&] {
        while (!stop) {
            OverlayLifecycle::Frame frame(lifecycle);
            if (!frame) { ++skipped; continue; }
            if (!Whole(context)) ++torn;
            std::this_thread::yield();
            if (!Whole(context)) ++torn;
            ++frames;
        }
    });
    std::thread window([&] {
        while (!stop) {
            const OverlayLifecycle::Message message(lifecycle);
            if (!Whole(context)) ++torn;
            ++messages;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop = true;
    game.join(); render.join(); window.join();
    delete context;

    std::printf("changes=%u frames=%u skipped=%u messages=%u torn=%u\n",
        changes.load(), frames.load(), skipped.load(), messages.load(), torn.load());
    Check(torn == 0, "a frame or message saw the context freed or half built");
    Check(changes > 0 && frames > 0 && messages > 0, "a role never ran");
}

// The thread replacing the context can be handed a window message (a display
// reset activates the window); it must not wait for itself.
static void AMessageDuringAChangeOnItsThreadPassesThrough() {
    OverlayLifecycle lifecycle;
    OverlayLifecycle::Change change(lifecycle);
    const OverlayLifecycle::Message message(lifecycle);
    Check(!message.Locked(), "a message on the changing thread took the lock again");
}

// A frame in progress holds a change off until it is drawn, and a message
// reaching the drawing thread in that time passes through instead of queueing
// behind the waiting change (which would deadlock).
static void AChangeWaitsForTheFrameInProgress() {
    OverlayLifecycle lifecycle;
    std::atomic<bool> changed{false};
    std::thread game;
    {
        OverlayLifecycle::Frame frame(lifecycle);
        Check(static_cast<bool>(frame), "an idle lifecycle refused a frame");
        game = std::thread([&] { OverlayLifecycle::Change change(lifecycle); changed = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Check(!changed, "the context was replaced under a frame");
        const auto start = Clock::now();
        const OverlayLifecycle::Message message(lifecycle);
        Check(Clock::now() - start < std::chrono::milliseconds(50), "a message on the drawing thread waited");
        Check(!message.Locked(), "a message on the drawing thread took the lock again");
    }
    game.join();
    Check(changed, "the change never ran after the frame");
}

// Handling a message can send another to the same window (ReleaseCapture
// sends WM_CAPTURECHANGED). With a change waiting for the outer message, the
// nested one must pass straight through rather than queue behind the change.
static void ANestedMessagePassesAWaitingChange() {
    OverlayLifecycle lifecycle;
    std::atomic<bool> changed{false};
    std::thread game;
    {
        const OverlayLifecycle::Message outer(lifecycle);
        Check(outer.Locked(), "an outer message did not take the lock");
        game = std::thread([&] { OverlayLifecycle::Change change(lifecycle); changed = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto start = Clock::now();
        {
            const OverlayLifecycle::Message nested(lifecycle);
            Check(!nested.Locked(), "a nested message took the lock again");
        }
        Check(Clock::now() - start < std::chrono::milliseconds(50), "a nested message waited for the change");
        Check(!changed, "the context was replaced under a message");
    }
    game.join();
    Check(changed, "the change never ran after the message");
    const OverlayLifecycle::Message later(lifecycle);
    Check(later.Locked(), "a message after the nested one did not take the lock");
}

// While a change runs, frames on other threads are skipped, not delayed.
static void AFrameDuringAChangeIsSkipped() {
    OverlayLifecycle lifecycle;
    OverlayLifecycle::Change change(lifecycle);
    bool drew = true;
    std::thread render([&] { OverlayLifecycle::Frame frame(lifecycle); drew = static_cast<bool>(frame); });
    render.join();
    Check(!drew, "a frame was drawn while the context was being replaced");
}

int main() {
    ThreeThreadsNeverSeeAFreedContext();
    AMessageDuringAChangeOnItsThreadPassesThrough();
    AChangeWaitsForTheFrameInProgress();
    ANestedMessagePassesAWaitingChange();
    AFrameDuringAChangeIsSkipped();
    if (g_failures) { std::printf("overlay lifecycle: %d check(s) failed\n", g_failures); return 1; }
    std::printf("overlay lifecycle: all checks passed\n");
    return 0;
}
