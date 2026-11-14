#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sf4e { namespace platform {
// The Windows file picker for replay and set pack files (ApplicationServices'
// PickReplays), over the game's window, in the Downloads folder. It runs on a
// thread of its own, so nothing else waits while it is open; what was picked is
// taken with Take, by the game thread, so a pick never lands anywhere else.
class ReplayPicker {
public:
    // choose: the files the player picks, or none, in place of the dialog (a
    // test has no dialog to open). It runs on the picker's own thread.
    explicit ReplayPicker(std::function<std::vector<std::wstring>()> choose = {});
    ~ReplayPicker();
    // The game's display as the game knows it: its window (which the picker
    // is shown over; null for none) and whether it owns the whole screen
    // (exclusive full screen), where Open is refused.
    void SetDisplay(void* window, bool exclusiveFullScreen);
    // The picker can be opened now: not while the game owns the whole screen,
    // where a window would stay behind it.
    bool CanOpen() const { return !exclusiveFullScreen_.load(); }
    // The picker is open now.
    bool IsOpen() const;
    // Opens the dialog; false when it cannot be now or one is already open.
    bool Open();
    // The files picked since the last call, as UTF-8 paths.
    std::vector<std::string> Take();
    // An open dialog is closed, not waited for without end: its window is asked
    // to close for a moment, and a thread that still has not ended is let go (it
    // holds only its own state, never this object). Open is refused after it.
    void Close();
private:
    // The picker's own thread and what it shares with the game thread. It
    // outlives a destroyed picker only as long as its thread does, so the
    // thread holds it and never this object.
    struct Thread;
    std::shared_ptr<Thread> Current() const;
    std::function<std::vector<std::wstring>()> choose_;
    mutable std::mutex mutex_;
    std::shared_ptr<Thread> thread_;
    bool closed_ = false;
    std::atomic<void*> window_{nullptr};
    std::atomic<bool> exclusiveFullScreen_{false};
};
} }
