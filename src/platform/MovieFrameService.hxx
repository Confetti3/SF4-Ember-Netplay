#pragma once

// The launcher's side of the menu backdrop's movie frames
// (MovieFrameMailbox.hxx): a thread beside the game that decodes one frame
// at a time with Media Foundation, when the game asks for one, into the
// cache folder. Not for the game's own process (MENU_BACKDROP_DESIGN): the
// decoder's modules and buffers would take the 32-bit game's address space.
// Media Foundation is delay-loaded and started only at the first request, so
// a session that never shows the backdrop, or a Windows without it (N
// editions without the Media Feature Pack, often Wine), loads nothing:
// every request is then answered Unavailable and the game falls back to its
// other pictures.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace sf4e { namespace platform { namespace movie_frames {

enum class Decoded { Ok, Failed, Unavailable };
// Decodes the frame of `movie` (a full path) at `ms`, or the first after it,
// fits it within 1280x720 at its own shape and writes it to `file` as a
// JPEG, through a temporary file beside it so a reader never sees half of
// one. Media Foundation must be started (MFStartup) on this thread's
// process, and COM initialized on the thread. `cancel`, when set, stops it
// between samples. Unavailable: no frame of any movie can be decoded here
// (no WMV decoder, no source for the container); Failed: this one could not.
Decoded DecodeFrame(const std::wstring& movie, std::uint32_t ms, const std::wstring& file, const std::atomic<bool>* cancel,
    std::string& why);

// One game's service: the mailbox named for its process id, served on a
// thread of its own, below normal priority, until Stop or destruction.
// Each request names a frame of movie_frames::Curated(), else it is
// refused (Failed); one already in the cache folder is Ready at once.
class MovieFrameService {
public:
    using Logger = std::function<void(const std::string&)>;
    MovieFrameService() = default;
    MovieFrameService(const MovieFrameService&) = delete;
    MovieFrameService& operator=(const MovieFrameService&) = delete;
    ~MovieFrameService() { Stop(); }
    // False (and nothing runs) when the mailbox could not be made; the game
    // then shows its other pictures. `mailbox` is for tests: empty takes the
    // game's own name (MailboxName); `cache` likewise (CacheFolder).
    bool Start(DWORD gamePid, std::wstring gameFolder, Logger log, std::wstring mailbox = std::wstring(),
        std::wstring cache = std::wstring());
    // Ends the thread; a decode under way stops at its next sample.
    void Stop();

private:
    std::wstring name_, gameFolder_, cache_;
    Logger log_;
    HANDLE stop_ = nullptr;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

} } }
