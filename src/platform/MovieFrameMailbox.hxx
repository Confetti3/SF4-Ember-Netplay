#pragma once

// The game asks Ember's launcher for one frame of the menu backdrop's movies
// (common/MovieFrames.hxx) and the launcher writes it, decoded, into the
// cache folder, where the game reads it as a picture. Decoding a video never
// happens in the 32-bit game: its decoder's modules and buffers would take
// address space the game runs short of (MENU_BACKDROP_DESIGN).
// The launcher creates, for each game it starts, a small named section, an
// auto-reset event and a mutex in this session's Local namespace, named by
// the game's process id. The game opens them the first time the backdrop
// wants a frame that is not in the cache yet; a game started without Ember's
// launcher finds none and shows its other pictures. One request at a time:
// the game writes the frame's movie and time and raises the request number,
// then reads the answer for that number on later frames without waiting. The
// launcher refuses a name another process created first, so all three it
// uses carry the default security of the player's own token; another process
// of the player's can still open them, but the launcher decodes only a
// frame of its own list (movie_frames::Find), and the game reads only a
// picture from the cache folder, so whatever another process writes into the
// slot can neither name a file nor reach the decoder with a path.

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace sf4e { namespace platform { namespace movie_frames {

inline std::wstring MailboxName(DWORD gamePid) { return L"Local\\SF4EmberMovieFrame-" + std::to_wstring(gamePid); }
inline std::wstring MailboxEventName(const std::wstring& name) { return name + L"Ready"; }
inline std::wstring MailboxLockName(const std::wstring& name) { return name + L"Lock"; }
inline bool MailboxLocked(DWORD wait) { return wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }

// None: no answer yet. Ready: the frame is in the cache folder under its
// FileName. Failed: this frame could not be decoded (its movie missing, a
// broken file, a request not on the list). Unavailable: no frame can be,
// this session (Media Foundation or its WMV decoder missing, as on Windows N
// without the Media Feature Pack and often under Wine).
enum class Answer : std::uint32_t { None = 0, Ready = 1, Failed = 2, Unavailable = 3 };
constexpr std::uint32_t SlotVersion = 1;
struct Slot {
    std::uint32_t version;  // SlotVersion, written by the launcher when it creates the slot
    std::uint32_t request;  // raised by the game with each request
    std::uint32_t answered; // the request the answer is for
    std::uint32_t answer;   // Answer
    std::uint32_t ms;
    char movie[128];
};

// %LOCALAPPDATA%\sf4e\backdrop-frames-1: the decoded frames, kept from one
// session to the next so a later one reads files. A cache: deleting it costs
// a decode. Empty when Windows does not say where Local AppData is.
inline std::wstring CacheFolder() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) {
        if (local) CoTaskMemFree(local);
        return std::wstring();
    }
    std::wstring folder = std::wstring(local) + L"\\sf4e\\backdrop-frames-1";
    CoTaskMemFree(local);
    return folder;
}

// The launcher's end, one game's. Create once; Next and Answer on the
// thread that serves it.
class MovieFrameHost {
public:
    MovieFrameHost() = default;
    MovieFrameHost(const MovieFrameHost&) = delete;
    MovieFrameHost& operator=(const MovieFrameHost&) = delete;
    ~MovieFrameHost() { Close(); }

    bool Create(const std::wstring& name) {
        Close();
        section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Slot), name.c_str());
        // A section of that name already there (a game of the same id from
        // before, still held): not this game's.
        if (section_ && GetLastError() == ERROR_ALREADY_EXISTS) { Close(); return false; }
        if (section_) slot_ = static_cast<Slot*>(MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Slot)));
        // The event and the lock must be new too: one another process made
        // first (with its own security, or held) is refused like the section.
        ready_ = CreateEventW(nullptr, FALSE, FALSE, MailboxEventName(name).c_str());
        const bool readyFound = ready_ && GetLastError() == ERROR_ALREADY_EXISTS;
        lock_ = CreateMutexW(nullptr, FALSE, MailboxLockName(name).c_str());
        const bool lockFound = lock_ && GetLastError() == ERROR_ALREADY_EXISTS;
        if (!slot_ || !ready_ || !lock_ || readyFound || lockFound) { Close(); return false; }
        SecureZeroMemory(slot_, sizeof(Slot));
        slot_->version = SlotVersion;
        return true;
    }
    HANDLE ReadyEvent() const { return ready_; }
    // Copies out the request in the slot, after its event was signalled.
    // False when the slot holds none (or the lock stayed taken).
    bool Take(std::uint32_t& request, std::string& movie, std::uint32_t& ms) {
        if (!slot_ || !MailboxLocked(WaitForSingleObject(lock_, 2000))) return false;
        char copy[sizeof(Slot::movie) + 1] = {};
        std::memcpy(copy, slot_->movie, sizeof(Slot::movie));
        request = slot_->request;
        ms = slot_->ms;
        const bool asked = request != 0 && slot_->answered != request;
        ReleaseMutex(lock_);
        movie = copy;
        return asked;
    }
    // The answer to `request`, kept only while the slot still holds it: a
    // newer request has its own.
    void Reply(std::uint32_t request, Answer answer) {
        if (!slot_ || !MailboxLocked(WaitForSingleObject(lock_, 2000))) return;
        if (slot_->request == request) {
            slot_->answered = request;
            slot_->answer = static_cast<std::uint32_t>(answer);
        }
        ReleaseMutex(lock_);
    }

private:
    void Close() {
        if (slot_) UnmapViewOfFile(slot_);
        if (section_) CloseHandle(section_);
        if (ready_) CloseHandle(ready_);
        if (lock_) CloseHandle(lock_);
        slot_ = nullptr; section_ = ready_ = lock_ = nullptr;
    }
    HANDLE section_ = nullptr, ready_ = nullptr, lock_ = nullptr;
    Slot* slot_ = nullptr;
};

// The game's end. Used from the thread that draws the overlay; never waits
// for the decode, and for the lock only briefly.
class MovieFrameClient {
public:
    MovieFrameClient() = default;
    MovieFrameClient(const MovieFrameClient&) = delete;
    MovieFrameClient& operator=(const MovieFrameClient&) = delete;
    ~MovieFrameClient() { Close(); }

    // False when no launcher made the mailbox for this game, or one of
    // another version did.
    bool Open(const std::wstring& name) {
        if (slot_) return true;
        section_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (section_) slot_ = static_cast<Slot*>(MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Slot)));
        ready_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, MailboxEventName(name).c_str());
        lock_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, MailboxLockName(name).c_str());
        if (!slot_ || !ready_ || !lock_ || slot_->version != SlotVersion) { Close(); return false; }
        return true;
    }
    bool Opened() const { return slot_ != nullptr; }
    // Asks for the frame of `movie` at `ms`; the request's number, or 0 when
    // it could not be sent (no mailbox, a path too long, the lock held).
    std::uint32_t Ask(const char* movie, std::uint32_t ms) {
        const std::size_t length = movie ? std::strlen(movie) : 0;
        if (!slot_ || !length || length >= sizeof(Slot::movie) || !MailboxLocked(WaitForSingleObject(lock_, 20))) return 0;
        std::uint32_t request = slot_->request + 1;
        if (!request) request = 1;
        SecureZeroMemory(slot_->movie, sizeof(Slot::movie));
        std::memcpy(slot_->movie, movie, length);
        slot_->ms = ms;
        slot_->request = request;
        const bool signalled = SetEvent(ready_) != FALSE;
        ReleaseMutex(lock_);
        return signalled ? request : 0;
    }
    // The answer to `request`; None while there is none yet, or while the
    // lock is held this frame.
    Answer Read(std::uint32_t request) {
        if (!slot_ || !request || !MailboxLocked(WaitForSingleObject(lock_, 0))) return Answer::None;
        Answer answer = Answer::None;
        if (slot_->answered == request) {
            const std::uint32_t raw = slot_->answer;
            answer = raw >= 1 && raw <= 3 ? static_cast<Answer>(raw) : Answer::Failed;
        }
        ReleaseMutex(lock_);
        return answer;
    }

private:
    void Close() {
        if (slot_) UnmapViewOfFile(slot_);
        if (section_) CloseHandle(section_);
        if (ready_) CloseHandle(ready_);
        if (lock_) CloseHandle(lock_);
        slot_ = nullptr; section_ = ready_ = lock_ = nullptr;
    }
    HANDLE section_ = nullptr, ready_ = nullptr, lock_ = nullptr;
    Slot* slot_ = nullptr;
};

} } }
