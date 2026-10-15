#include "DurableFile.hxx"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace sf4e { namespace durable {
namespace {
WriteResult Publish(const std::filesystem::path& path, const std::filesystem::path& temporary,
    const void* data, std::size_t size, DWORD flags) {
    const auto written = WriteNew(temporary, data, size);
    if (!written) return written;
    if (MoveFileExW(temporary.c_str(), path.c_str(), flags)) return {};
    const DWORD error = GetLastError();
    DeleteFileW(temporary.c_str());
    return {WriteStep::Move, error};
}
}

WriteResult WriteNew(const std::filesystem::path& path, const void* data, std::size_t size) {
    // One WriteFile call takes at most a DWORD; refuse before creating anything.
    if (static_cast<std::uint64_t>(size) > MAXDWORD) return {WriteStep::Write, ERROR_FILE_TOO_LARGE};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {WriteStep::Create, GetLastError()};
    // Each failure keeps its own code; a short write that reports success sets none.
    DWORD written = 0, error = ERROR_SUCCESS;
    if (!WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr)) error = GetLastError();
    else if (written != size) error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && error == ERROR_SUCCESS) error = GetLastError();
    if (error == ERROR_SUCCESS) return {};
    DeleteFileW(path.c_str());
    return {WriteStep::Write, error};
}

WriteResult PublishCreateOnly(const std::filesystem::path& path, const std::filesystem::path& temporary,
    const void* data, std::size_t size) {
    return Publish(path, temporary, data, size, MOVEFILE_WRITE_THROUGH);
}

WriteResult PublishReplace(const std::filesystem::path& path, const std::filesystem::path& temporary,
    const void* data, std::size_t size) {
    return Publish(path, temporary, data, size, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::filesystem::path PartialPath(const std::filesystem::path& path) {
    static std::atomic<unsigned> serial{0};
    return path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial) + L".tmp";
}

} }
