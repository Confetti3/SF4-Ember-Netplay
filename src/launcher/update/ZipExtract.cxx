#include "ZipExtract.hxx"
#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace sf4e { namespace launcher {
namespace {
namespace fs = std::filesystem;

std::uint32_t U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t U16(const unsigned char* p) { return std::uint16_t(p[0] | (p[1] << 8)); }

struct File {
    FILE* f = nullptr;
    ~File() { if (f) fclose(f); }
    bool ReadAt(std::uint64_t at, void* out, size_t n) {
        return _fseeki64(f, static_cast<long long>(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n;
    }
};

bool SafeName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) return false;
    for (size_t at = 0; at <= name.size();) {
        size_t slash = name.find('/', at);
        if (slash == std::string::npos) slash = name.size();
        const auto part = name.substr(at, slash - at);
        if ((part.empty() && slash != name.size()) || part == "." || part == "..") return false;
        at = slash + 1;
    }
    return true;
}

// Copies one entry's data to out, inflating when deflated, and checks the CRC.
std::string Copy(File& in, std::uint64_t at, std::uint32_t packed, std::uint32_t size, std::uint16_t method,
    std::uint32_t crc, FILE* out, std::uint64_t& done, std::uint64_t total,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    std::vector<unsigned char> buffer(1 << 16), inflated(1 << 16);
    z_stream stream{};
    if (method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) return "inflate init failed";
    std::uint32_t actual = crc32(0, nullptr, 0), written = 0;
    std::string error;
    for (std::uint32_t left = packed; left > 0 || (method == 8 && written < size);) {
        const size_t take = static_cast<size_t>(left < buffer.size() ? left : buffer.size());
        if (take == 0) { error = "deflate stream ended early"; break; }
        if (!in.ReadAt(at, buffer.data(), take)) { error = "read failed"; break; }
        at += take; left -= static_cast<std::uint32_t>(take);
        if (method == 0) {
            if (fwrite(buffer.data(), 1, take, out) != take) { error = "write failed"; break; }
            actual = crc32(actual, buffer.data(), static_cast<uInt>(take)); written += static_cast<std::uint32_t>(take);
        } else {
            stream.next_in = buffer.data(); stream.avail_in = static_cast<uInt>(take);
            int code = Z_OK;
            while (stream.avail_in > 0 && code != Z_STREAM_END) {
                stream.next_out = inflated.data(); stream.avail_out = static_cast<uInt>(inflated.size());
                code = inflate(&stream, Z_NO_FLUSH);
                if (code != Z_OK && code != Z_STREAM_END) { error = "inflate failed"; break; }
                const size_t got = inflated.size() - stream.avail_out;
                if (fwrite(inflated.data(), 1, got, out) != got) { error = "write failed"; break; }
                actual = crc32(actual, inflated.data(), static_cast<uInt>(got)); written += static_cast<std::uint32_t>(got);
            }
            if (!error.empty()) break;
            if (code == Z_STREAM_END) break;
        }
        done += take;
        if (progress && !progress(done, total)) { error = "cancelled"; break; }
    }
    if (method == 8) inflateEnd(&stream);
    if (!error.empty()) return error;
    if (written != size) return "entry size mismatch";
    if (actual != crc) return "entry crc mismatch";
    return {};
}
}

std::string ExtractZip(const std::wstring& zipPath, const std::wstring& destDir,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    File in;
    if (_wfopen_s(&in.f, zipPath.c_str(), L"rb") != 0 || !in.f) return "cannot open zip";
    _fseeki64(in.f, 0, SEEK_END);
    const std::uint64_t length = static_cast<std::uint64_t>(_ftelli64(in.f));
    // End of central directory: 22 bytes plus a comment of at most 65535.
    const size_t tail = static_cast<size_t>(length < 65557 ? length : 65557);
    std::vector<unsigned char> end(tail);
    if (tail < 22 || !in.ReadAt(length - tail, end.data(), tail)) return "not a zip";
    size_t eocd = tail - 22;
    while (U32(&end[eocd]) != 0x06054b50) { if (eocd == 0) return "zip directory not found"; --eocd; }
    const std::uint16_t entries = U16(&end[eocd + 10]);
    std::uint64_t at = U32(&end[eocd + 16]);
    if (entries == 0xffff || at == 0xffffffff) return "zip64 is not supported";

    struct Entry { std::string name; std::uint16_t method; std::uint32_t crc, packed, size, local; };
    std::vector<Entry> list;
    std::uint64_t total = 0;
    for (std::uint16_t i = 0; i < entries; ++i) {
        unsigned char h[46];
        if (!in.ReadAt(at, h, sizeof h) || U32(h) != 0x02014b50) return "bad central directory";
        Entry e{ {}, U16(h + 10), U32(h + 16), U32(h + 20), U32(h + 24), U32(h + 42) };
        const std::uint16_t nameLength = U16(h + 28);
        e.name.resize(nameLength);
        if (!in.ReadAt(at + 46, e.name.data(), nameLength)) return "bad central directory";
        if (!SafeName(e.name)) return "unsafe entry name: " + e.name;
        if (e.method != 0 && e.method != 8) return "unsupported compression: " + e.name;
        at += 46 + nameLength + U16(h + 30) + U16(h + 32);
        total += e.packed;
        list.push_back(std::move(e));
    }

    const fs::path root(destDir);
    std::uint64_t done = 0;
    for (const auto& e : list) {
        const fs::path target = root / fs::u8path(e.name);
        std::error_code ec;
        if (e.name.back() == '/') { fs::create_directories(target, ec); continue; }
        fs::create_directories(target.parent_path(), ec);
        unsigned char local[30];
        if (!in.ReadAt(e.local, local, sizeof local) || U32(local) != 0x04034b50) return "bad local header: " + e.name;
        const std::uint64_t data = e.local + 30 + U16(local + 26) + U16(local + 28);
        FILE* out = nullptr;
        if (_wfopen_s(&out, target.c_str(), L"wb") != 0 || !out) return "cannot create: " + e.name;
        const std::string error = Copy(in, data, e.packed, e.size, e.method, e.crc, out, done, total, progress);
        fclose(out);
        if (!error.empty()) return error + ": " + e.name;
    }
    return {};
}
} }
