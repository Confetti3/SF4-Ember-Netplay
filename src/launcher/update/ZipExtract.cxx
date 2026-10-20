#include "ZipExtract.hxx"
#include "PackageInstaller.hxx"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sf4e { namespace launcher {
namespace {
namespace fs = std::filesystem;

std::uint32_t U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t U16(const unsigned char* p) { return std::uint16_t(p[0] | (p[1] << 8)); }

struct Entry { std::string name; std::uint16_t method; std::uint32_t crc, packed, size, local; };

bool ReadAt(std::ifstream& in, std::uint64_t at, void* out, std::size_t n) {
    in.clear();
    return static_cast<bool>(in.seekg(static_cast<std::streamoff>(at)).read(static_cast<char*>(out), n));
}

// Copies the entry's data starting at `at` to out, inflating it when deflated,
// and checks its size and CRC.
std::string Copy(std::ifstream& in, std::uint64_t at, const Entry& e, std::ofstream& out, const std::atomic<bool>& cancel) {
    std::vector<unsigned char> buffer(1 << 16), inflated(1 << 16);
    z_stream stream{};
    if (e.method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) return "inflate init failed";
    std::uint32_t crc = crc32(0, nullptr, 0), written = 0;
    const auto put = [&](const unsigned char* data, std::size_t n) {
        out.write(reinterpret_cast<const char*>(data), n);
        crc = crc32(crc, data, static_cast<uInt>(n)); written += static_cast<std::uint32_t>(n);
        return static_cast<bool>(out);
    };
    std::string error;
    for (std::uint32_t left = e.packed; left > 0 || (e.method == 8 && written < e.size);) {
        const std::size_t take = left < buffer.size() ? left : buffer.size();
        if (take == 0) { error = "deflate stream ended early"; break; }
        if (!ReadAt(in, at, buffer.data(), take)) { error = "read failed"; break; }
        at += take; left -= static_cast<std::uint32_t>(take);
        if (e.method == 0) {
            if (!put(buffer.data(), take)) { error = "write failed"; break; }
        } else {
            stream.next_in = buffer.data(); stream.avail_in = static_cast<uInt>(take);
            int code = Z_OK;
            // inflate can take the last input bytes while output is still
            // pending, so a call that fills the whole buffer is followed by
            // another even when no input is left.
            while ((stream.avail_in > 0 || stream.avail_out == 0) && code != Z_STREAM_END) {
                stream.next_out = inflated.data(); stream.avail_out = static_cast<uInt>(inflated.size());
                code = inflate(&stream, Z_NO_FLUSH);
                if (code == Z_BUF_ERROR) break; // nothing was pending: the next chunk is needed
                if (code != Z_OK && code != Z_STREAM_END) { error = "inflate failed"; break; }
                if (!put(inflated.data(), inflated.size() - stream.avail_out)) { error = "write failed"; break; }
            }
            if (!error.empty() || code == Z_STREAM_END) break;
        }
        if (cancel) { error = "cancelled"; break; }
    }
    if (e.method == 8) inflateEnd(&stream);
    if (!error.empty()) return error;
    if (written != e.size) return "entry size mismatch";
    if (crc != e.crc) return "entry crc mismatch";
    return {};
}
}

bool ExtractZip(const std::wstring& zipPath, const std::wstring& destDir, std::string& error, const std::atomic<bool>& cancel) {
    const auto fail = [&](std::string why) { error = std::move(why); return false; };
    const fs::path root(destDir);
    std::ifstream in(fs::path(zipPath), std::ios::binary | std::ios::ate);
    if (!in) return fail("cannot open zip");
    const std::uint64_t length = static_cast<std::uint64_t>(in.tellg());
    // End of central directory: 22 bytes plus a comment of at most 65535.
    const std::size_t tail = static_cast<std::size_t>(length < 65557 ? length : 65557);
    std::vector<unsigned char> end(tail);
    if (tail < 22 || !ReadAt(in, length - tail, end.data(), tail)) return fail("not a zip");
    std::size_t eocd = tail - 22;
    while (U32(&end[eocd]) != 0x06054b50) { if (eocd == 0) return fail("zip directory not found"); --eocd; }
    const std::uint16_t entries = U16(&end[eocd + 10]);
    std::uint64_t at = U32(&end[eocd + 16]);
    if (entries == 0xffff || at == 0xffffffff) return fail("zip64 is not supported");

    std::vector<Entry> list;
    for (std::uint16_t i = 0; i < entries; ++i) {
        unsigned char h[46];
        if (!ReadAt(in, at, h, sizeof h) || U32(h) != 0x02014b50) return fail("bad central directory");
        Entry e{ {}, U16(h + 10), U32(h + 16), U32(h + 20), U32(h + 24), U32(h + 42) };
        e.name.resize(U16(h + 28));
        if (!ReadAt(in, at + 46, e.name.data(), e.name.size())) return fail("bad central directory");
        if (e.method != 0 && e.method != 8) return fail("unsupported compression: " + e.name);
        at += 46 + e.name.size() + U16(h + 30) + U16(h + 32);
        list.push_back(std::move(e));
    }
    for (const auto& e : list) {
        // Windows PowerShell's Compress-Archive writes folders with a trailing backslash.
        const bool folder = !e.name.empty() && (e.name.back() == '/' || e.name.back() == '\\');
        const auto relative = fs::u8path(folder ? e.name.substr(0, e.name.size() - 1) : e.name);
        try { CheckPath(root, relative); } catch (const std::exception&) { return fail("unsafe entry name: " + e.name); }
        const fs::path target = root / relative;
        std::error_code ignored;
        fs::create_directories(folder ? target : target.parent_path(), ignored);
        if (folder) continue;
        unsigned char local[30];
        if (!ReadAt(in, e.local, local, sizeof local) || U32(local) != 0x04034b50) return fail("bad local header: " + e.name);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return fail("cannot create: " + e.name);
        const std::string copied = Copy(in, e.local + 30 + U16(local + 26) + U16(local + 28), e, out, cancel);
        if (!copied.empty()) return fail(copied + ": " + e.name);
    }
    return true;
}
} }

