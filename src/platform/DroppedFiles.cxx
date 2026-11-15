#include "DroppedFiles.hxx"
#include "Utf8.hxx"
#include "../common/ReplayImportQueue.hxx"
#include <windows.h>
#include <shellapi.h>

namespace sf4e { namespace platform {

void AcceptDroppedFiles(void* window, bool accept) {
    if (window) DragAcceptFiles(static_cast<HWND>(window), accept ? TRUE : FALSE);
}

std::vector<std::string> ReadDroppedFiles(void* handle) {
    std::vector<std::string> files;
    const HDROP drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count && i < replay_import::kMostWaiting; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (!length || length > 2048) continue;
        std::wstring name(length + 1, L' ');
        name.resize(DragQueryFileW(drop, i, &name[0], length + 1));
        files.push_back(WideToUtf8(name));
    }
    DragFinish(drop);
    return files;
}

} }
