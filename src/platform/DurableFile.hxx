#pragma once
#include "../common/BoundedRead.hxx"
#include <cstddef>
#include <filesystem>

// Durable writes for files that must never be seen half written: replay
// archives, settings and the updater's journal. Windows only; the reader in
// BoundedRead.hxx is the portable half.
namespace sf4e { namespace durable {

// Where a write stopped, with the system error there, so each caller keeps its
// own messages and can retry a temporary name that was already taken.
enum class WriteStep { Done, Create, Write, Move };
struct WriteResult {
    WriteStep failed = WriteStep::Done;
    unsigned long error = 0;
    explicit operator bool() const { return failed == WriteStep::Done; }
};

// Creates `path`, which must not exist yet, holding `size` bytes flushed to
// disk. A file this call created but could not complete is removed.
WriteResult WriteNew(const std::filesystem::path& path, const void* data, std::size_t size);

// Writes `temporary` as WriteNew does, then renames it onto `path`, never
// replacing a `path` that exists: a destination that appears meanwhile wins.
// The temporary is removed whenever the call fails after creating it.
WriteResult PublishCreateOnly(const std::filesystem::path& path, const std::filesystem::path& temporary,
    const void* data, std::size_t size);
// The same, replacing `path` if it exists.
WriteResult PublishReplace(const std::filesystem::path& path, const std::filesystem::path& temporary,
    const void* data, std::size_t size);

// A temporary name beside `path` that this process has not used before.
std::filesystem::path PartialPath(const std::filesystem::path& path);

} }
