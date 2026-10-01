#pragma once

// Who may touch the overlay's ImGui context. SF4 creates and frees it on the
// game thread (around every D3D Reset), draws it on its render thread and can
// deliver window messages on either. A frame and a window message share the
// context; creating or freeing it excludes both, so a Reset can no longer free
// the context, its fonts or the portrait textures under a frame being drawn.
//
// A frame never waits: while the context is being replaced it is skipped. A
// message does wait, since the input bridge must see every key release
// (dropping them is what the input bridge replaced). A thread that holds the
// context already, replacing it or drawing a frame, passes straight through
// when a window message reaches it, so neither re-enters the lock.

#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace sf4e { namespace ui {

class OverlayLifecycle {
public:
    // Held while the context is created or freed.
    class Change {
    public:
        explicit Change(OverlayLifecycle& lifecycle) : lock_(lifecycle.mutex_) { depth_ = 1; }
        ~Change() { depth_ = 0; }
        Change(const Change&) = delete;
        Change& operator=(const Change&) = delete;
    private:
        std::unique_lock<std::shared_mutex> lock_;
    };

    // Held for a whole frame. False when the context is being replaced.
    class Frame {
    public:
        explicit Frame(OverlayLifecycle& lifecycle) : lock_(lifecycle.mutex_, std::try_to_lock) {
            if (lock_.owns_lock()) depth_ = 1;
        }
        ~Frame() { if (lock_.owns_lock()) depth_ = 0; }
        explicit operator bool() const { return lock_.owns_lock(); }
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
    private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    // Held while one window message is handled.
    std::shared_lock<std::shared_mutex> Message() {
        if (depth_) return {};
        return std::shared_lock<std::shared_mutex>(mutex_);
    }

private:
    std::shared_mutex mutex_;
    // Whether this thread already holds the context (a Change or a Frame).
    static inline thread_local int depth_ = 0;
};

} }
