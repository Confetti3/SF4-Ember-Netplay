#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

// The whole-file reader under replays, training files and settings. Portable,
// so game-independent code and tests use it too; the durable writers are in
// platform/DurableFile.hxx.
namespace sf4e { namespace durable {

// Missing is only a path that does not exist. A folder or other non-file, a
// file that cannot be opened and an interrupted read are failures, never
// "missing", so a caller cannot mistake them for a free name.
enum class ReadStatus { Read, Missing, CannotOpen, TooLarge, CannotRead };
struct ReadResult {
    ReadStatus status = ReadStatus::CannotOpen;
    std::vector<std::uint8_t> bytes; // The whole file when Read; it may be empty.
};

// Reads a whole file of at most `most` bytes. The bound applies to what is
// read, not to the size reported beforehand, so a file that grows meanwhile is
// still bounded.
inline ReadResult ReadBounded(const std::filesystem::path& path, std::size_t most) {
    ReadResult result;
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory)) { result.status = ReadStatus::Missing; return result; }
    if (error || !std::filesystem::is_regular_file(status)) return result;
    std::ifstream file(path, std::ios::binary);
    if (!file) return result;
    constexpr std::size_t chunk = 64 * 1024;
    auto& bytes = result.bytes;
    for (;;) {
        // One byte past the bound tells an oversized file from one exactly at it.
        const std::size_t have = bytes.size(), want = most - have < chunk ? most - have + 1 : chunk;
        bytes.resize(have + want);
        file.read(reinterpret_cast<char*>(bytes.data() + have), static_cast<std::streamsize>(want));
        bytes.resize(have + static_cast<std::size_t>(file.gcount()));
        if (bytes.size() > most) result.status = ReadStatus::TooLarge;
        else if (file.bad() || (!file.eof() && file.fail())) result.status = ReadStatus::CannotRead;
        else if (file.eof()) { result.status = ReadStatus::Read; return result; }
        else continue;
        bytes.clear();
        return result;
    }
}

} }
