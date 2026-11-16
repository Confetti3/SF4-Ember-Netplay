#pragma once
#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <vector>

// The one place that drives zlib's inflate. The game's archives (EMZ, the font
// library), Ember's own textures and the launcher's zip entries all inflate
// through it, so each caller states a bound instead of keeping a loop of its own.
namespace sf4e { namespace zinflate {

// Raw is a bare deflate stream (an EMZ body, a zip entry); Zlib has the zlib
// header and checksum (PNG's data, a CFX font library).
enum class Format { Raw, Zlib };
// More: the input is used up and the stream has not ended. End: the stream is
// complete. Error: the data does not inflate. Refused: the sink returned false.
enum class Status { More, End, Error, Refused };
constexpr std::size_t AnySize = static_cast<std::size_t>(-1);

class Stream {
public:
    explicit Stream(Format format) : ready_(inflateInit2(&stream_, format == Format::Raw ? -MAX_WBITS : MAX_WBITS) == Z_OK), out_(1 << 16) {}
    ~Stream() { if (ready_) inflateEnd(&stream_); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    bool Ready() const { return ready_; }
    // Feeds `size` more input bytes and hands each piece of output to
    // `sink(const unsigned char*, std::size_t) -> bool`. A call that fills the
    // whole output buffer is followed by another even when no input is left,
    // since inflate can take the last input while output is still pending.
    template <class Sink>
    Status Feed(const unsigned char* data, std::size_t size, Sink&& sink) {
        if (!ready_) return Status::Error;
        stream_.next_in = const_cast<unsigned char*>(data); stream_.avail_in = static_cast<uInt>(size);
        int code = Z_OK;
        while ((stream_.avail_in > 0 || stream_.avail_out == 0) && code != Z_STREAM_END) {
            stream_.next_out = out_.data(); stream_.avail_out = static_cast<uInt>(out_.size());
            code = inflate(&stream_, Z_NO_FLUSH);
            if (code == Z_BUF_ERROR) break;  // nothing was pending: the next input is needed
            if (code != Z_OK && code != Z_STREAM_END) return Status::Error;
            if (!sink(out_.data(), out_.size() - stream_.avail_out)) return Status::Refused;
        }
        return code == Z_STREAM_END ? Status::End : Status::More;
    }
private:
    z_stream stream_{};
    bool ready_;
    std::vector<unsigned char> out_;
};

// All of `data` inflated into `out`: true only when the stream ends inside the
// input, yields at most `most` bytes, and, when `expect` is given, exactly
// that many. `out` is empty otherwise.
inline bool InflateBounded(const unsigned char* data, std::size_t size, Format format, std::size_t most, std::vector<unsigned char>& out,
    std::size_t expect = AnySize) {
    out.clear();
    if (expect != AnySize && expect > most) return false;
    Stream stream(format);
    if (!stream.Ready()) return false;
    if (expect != AnySize) out.reserve(expect);
    const Status status = stream.Feed(data, size, [&](const unsigned char* piece, std::size_t count) {
        if (count > most - out.size()) return false;
        out.insert(out.end(), piece, piece + count);
        return true;
    });
    if (status != Status::End || (expect != AnySize && out.size() != expect)) { out.clear(); return false; }
    return true;
}

} }
