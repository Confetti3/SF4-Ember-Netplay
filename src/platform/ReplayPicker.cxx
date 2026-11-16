#include "ReplayPicker.hxx"
#include "Utf8.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayImportQueue.hxx"
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

namespace {
// The Windows file picker: in Downloads, for replay and set pack files, any
// number of them. It blocks until the player closes it, so it only ever runs
// on the picker's own thread (ReplayPicker::Open).
//
// Its owner is a window made on this thread, an invisible one laid over the
// game's window. An owner that belongs to the game's thread would join the two
// threads' input queues for as long as the dialog is open, and a stall in
// either would then stall the other. This owner is the dialog's alone: the
// dialog still shows over the game and is modal against the clicks on it, and
// the game goes on drawing and taking its messages.
//
// The owner must be seen by the mouse and by the taskbar. A layered window of
// alpha 0 lets every click through to the game below, which is not disabled
// then, so the game would come to the front and bury the dialog; alpha 1 is
// as good as invisible and still takes the click, which lands on a disabled
// owner and brings the dialog forward. It is also an app window (no tool
// window, no no-activate), named like the dialog, so the taskbar and Alt+Tab
// have a way back to a dialog that has fallen behind.
std::vector<std::wstring> ChooseReplayFiles(const std::wstring& title, const std::wstring& kind, void* gameWindow) {
    std::vector<std::wstring> files;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            dialog->SetOptions(FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
            dialog->SetTitle(title.c_str());
            const COMDLG_FILTERSPEC filter[] = {{kind.c_str(), L"*.emberreplay;*.usf4replay;*.emberpack"}};
            dialog->SetFileTypes(1, filter);
            PWSTR downloads = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &downloads))) {
                Microsoft::WRL::ComPtr<IShellItem> folder;
                if (SUCCEEDED(SHCreateItemFromParsingName(downloads, nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
                CoTaskMemFree(downloads);
            }
            HWND owner = nullptr;
            RECT place{};
            if (gameWindow && IsWindow(static_cast<HWND>(gameWindow)) && GetWindowRect(static_cast<HWND>(gameWindow), &place)) {
                owner = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, L"STATIC", title.c_str(), WS_POPUP,
                    place.left, place.top, place.right - place.left, place.bottom - place.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (owner) { SetLayeredWindowAttributes(owner, 0, 1, LWA_ALPHA); ShowWindow(owner, SW_SHOWNA); }
            }
            if (SUCCEEDED(dialog->Show(owner))) {
                Microsoft::WRL::ComPtr<IShellItemArray> results;
                if (SUCCEEDED(dialog->GetResults(&results))) {
                    DWORD count = 0;
                    results->GetCount(&count);
                    for (DWORD i = 0; i < count && files.size() < sf4e::replay_import::kMostWaiting; ++i) {
                        Microsoft::WRL::ComPtr<IShellItem> item;
                        PWSTR name = nullptr;
                        if (SUCCEEDED(results->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                            files.push_back(name);
                            CoTaskMemFree(name);
                        }
                    }
                }
            }
            if (owner) DestroyWindow(owner);
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return files;
}
}

namespace sf4e { namespace platform {
struct ReplayPicker::Thread {
    std::mutex mutex;
    std::condition_variable ended;
    bool open = true;
    std::atomic<bool> cancelled{false};
    std::atomic<DWORD> threadId{0};
    std::vector<std::string> taken;
    std::thread thread;
};
ReplayPicker::ReplayPicker(std::function<std::vector<std::wstring>()> choose) : choose_(std::move(choose)) {}
ReplayPicker::~ReplayPicker() { Close(); }
void ReplayPicker::SetDisplay(void* window, bool exclusiveFullScreen) {
    window_ = window; exclusiveFullScreen_ = exclusiveFullScreen;
}
bool ReplayPicker::Open() {
    if (exclusiveFullScreen_.load()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    if (thread_) {
        { std::lock_guard<std::mutex> inner(thread_->mutex); if (thread_->open) return false; }
        thread_->thread.join();  // it has ended: its last step is clearing `open`
    }
    auto state = std::make_shared<Thread>();
    auto choose = choose_;
    const std::wstring title = Utf8ToWide(loc::T("replays.import_title"));
    const std::wstring kind = Utf8ToWide(loc::T("replays.import_filter"));
    void* const window = window_.load();
    try {
        state->thread = std::thread([state, choose, title, kind, window] {
            state->threadId = GetCurrentThreadId();
            std::vector<std::wstring> files;
            if (!state->cancelled) files = choose ? choose() : ChooseReplayFiles(title, kind, window);
            std::lock_guard<std::mutex> guard(state->mutex);
            if (!state->cancelled) for (const auto& file : files) state->taken.push_back(WideToUtf8(file));
            state->open = false;
            state->ended.notify_all();
        });
    } catch (const std::system_error&) { return false; }
    thread_ = std::move(state);
    return true;
}
std::shared_ptr<ReplayPicker::Thread> ReplayPicker::Current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_;
}
bool ReplayPicker::IsOpen() const {
    const auto state = Current();
    if (!state) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->open;
}
std::vector<std::string> ReplayPicker::Take() {
    const auto state = Current();
    if (!state) return {};
    std::lock_guard<std::mutex> lock(state->mutex);
    return std::exchange(state->taken, {});
}
void ReplayPicker::Close() {
    std::shared_ptr<Thread> state;
    { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; state = std::move(thread_); }
    if (!state) return;
    state->cancelled = true;
    std::unique_lock<std::mutex> lock(state->mutex);
    for (int tries = 0; state->open && tries < 30; ++tries) {
        if (const DWORD id = state->threadId) EnumThreadWindows(id, [](HWND window, LPARAM) -> BOOL { PostMessageW(window, WM_CLOSE, 0, 0); return TRUE; }, 0);
        state->ended.wait_for(lock, std::chrono::milliseconds(50), [&] { return !state->open; });
    }
    const bool ended = !state->open;
    lock.unlock();
    if (ended) state->thread.join(); else state->thread.detach();
}
} }
