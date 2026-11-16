// The shared inflate helper (Inflate.hxx): a stream inflated whole under a
// bound, fed in pieces to a sink, and refused when it is cut short, corrupt,
// too long or not the size expected.
#include "../common/Inflate.hxx"
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace zi = sf4e::zinflate;
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}
std::vector<unsigned char> Deflate(const std::vector<unsigned char>& raw, int windowBits) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    std::vector<unsigned char> out(deflateBound(&z, static_cast<uLong>(raw.size())) + 16);
    z.next_in = const_cast<unsigned char*>(raw.data()); z.avail_in = static_cast<uInt>(raw.size());
    z.next_out = out.data(); z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}
}

int main() {
    std::vector<unsigned char> raw;
    for (int i = 0; i < 200000; ++i) raw.push_back(static_cast<unsigned char>((i * 7 + i / 300) & 0xff));
    const auto zlibPacked = Deflate(raw, 15), rawPacked = Deflate(raw, -15);
    std::vector<unsigned char> out;
    Check(zi::InflateBounded(zlibPacked.data(), zlibPacked.size(), zi::Format::Zlib, raw.size(), out) && out == raw, "a zlib stream did not inflate to its bytes");
    Check(zi::InflateBounded(rawPacked.data(), rawPacked.size(), zi::Format::Raw, raw.size(), out, raw.size()) && out == raw, "a raw stream of the expected size did not inflate");
    Check(!zi::InflateBounded(rawPacked.data(), rawPacked.size(), zi::Format::Raw, raw.size() - 1, out) && out.empty(), "a stream longer than the bound inflated");
    Check(!zi::InflateBounded(rawPacked.data(), rawPacked.size(), zi::Format::Raw, raw.size(), out, raw.size() - 1) && out.empty(), "a stream not the expected size inflated");
    Check(!zi::InflateBounded(rawPacked.data(), rawPacked.size(), zi::Format::Raw, raw.size(), out, raw.size() + 1) && out.empty(), "a short stream passed for a longer size");
    Check(!zi::InflateBounded(rawPacked.data(), rawPacked.size() / 2, zi::Format::Raw, raw.size(), out) && out.empty(), "a cut-short stream inflated");
    auto corrupt = zlibPacked; corrupt[corrupt.size() / 2] ^= 0xff; corrupt[2] ^= 0xff;
    Check(!zi::InflateBounded(corrupt.data(), corrupt.size(), zi::Format::Zlib, raw.size(), out), "a corrupt stream inflated");
    Check(!zi::InflateBounded(rawPacked.data(), rawPacked.size(), zi::Format::Zlib, raw.size(), out), "a raw stream passed as zlib");
    // Fed in small pieces, the output arrives whole and the end is reported once.
    zi::Stream stream(zi::Format::Raw);
    std::vector<unsigned char> joined;
    auto status = zi::Status::More;
    for (std::size_t at = 0; at < rawPacked.size() && status == zi::Status::More; at += 100)
        status = stream.Feed(rawPacked.data() + at, (std::min<std::size_t>)(100, rawPacked.size() - at), [&](const unsigned char* data, std::size_t n) { joined.insert(joined.end(), data, data + n); return true; });
    Check(status == zi::Status::End && joined == raw, "a stream fed in pieces did not end whole");
    zi::Stream refusing(zi::Format::Raw);
    Check(refusing.Feed(rawPacked.data(), rawPacked.size(), [](const unsigned char*, std::size_t) { return false; }) == zi::Status::Refused, "a sink's refusal was not reported");
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("Inflate helper checks passed.\n");
    return 0;
}
