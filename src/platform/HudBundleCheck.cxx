#include "HudBundleCheck.hxx"
#include "Utf8.hxx"
#include "../common/BoundedRead.hxx"
#include "../common/Inflate.hxx"
#include <cstring>
#include <map>

namespace fs = std::filesystem;

namespace sf4e { namespace platform { namespace hud_bundle {
namespace {
using Bytes = std::vector<std::uint8_t>;
using game_themes::Verdict;
// A HUD file is a few megabytes at most.
constexpr std::size_t MostFile = 32u << 20;
constexpr std::size_t MostPairs = 1024;
constexpr std::uint32_t MostElements = 8192, MostKeys = 8192, MostPictures = 4096;
constexpr wchar_t Layout[] = L".ems.emz", Sheet[] = L".tex.emz";
constexpr std::size_t SuffixSize = 8;

// Every read is bounds-checked, and every offset sum is made in 64 bits.
bool Fits(const Bytes& d, std::uint64_t at, std::uint64_t size) { return at <= d.size() && size <= d.size() - at; }
bool U32(const Bytes& d, std::uint64_t at, std::uint32_t& v) {
    if (!Fits(d, at, 4)) return false;
    std::memcpy(&v, d.data() + at, 4);
    return true;
}
bool S16(const Bytes& d, std::uint64_t at, std::int16_t& v) {
    if (!Fits(d, at, 2)) return false;
    std::memcpy(&v, d.data() + at, 2);
    return true;
}
bool Is(const Bytes& d, const char* magic) { return Fits(d, 0, 4) && std::memcmp(d.data(), magic, 4) == 0; }

// The bundle inside a file: the file itself when it is one, or what an #EMZ wrapper holds
// (its size at +8 and the offset of its deflate stream at +0xC).
bool Unwrap(const Bytes& file, const char* magic, Bytes& body) {
    if (Is(file, magic)) { body = file; return true; }
    std::uint32_t size = 0, offset = 0;
    if (!Is(file, "#EMZ") || !U32(file, 8, size) || !U32(file, 12, offset)) return false;
    if (offset < 16 || offset > file.size() || size > MostFile) return false;
    return zinflate::InflateBounded(file.data() + offset, file.size() - offset, zinflate::Format::Raw, size, body, size) && Is(body, magic);
}
}

bool SheetPictures(const Bytes& file, std::uint32_t& count) {
    Bytes emb;
    std::uint32_t table = 0;
    if (!Unwrap(file, "#EMB", emb) || emb.size() < 0x20 || !U32(emb, 0xC, count) || !U32(emb, 0x18, table)) return false;
    return count <= MostPictures && Fits(emb, table, 8ull * count);
}

bool LayoutHighestPicture(const Bytes& file, std::int64_t& highest) {
    Bytes ems;
    std::uint32_t count = 0, table = 0;
    highest = -1;
    if (!Unwrap(file, "#EMS", ems) || ems.size() < 0x20 || !U32(ems, 0x18, count) || !U32(ems, 0x1C, table)) return false;
    if (count > MostElements || !Fits(ems, table, 4ull * count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        // An element's picture block (+8), whose key count is an int16 at +4, whose key frames sit
        // where +0x10 says and whose keys, 32 bytes each, where +0x14 says. Every offset counts from
        // the start of the file. The picture number is the first int32 of a key. The game plays the
        // keys only when the block has key frames (SSFIV.exe 0x6EDEC0 sets up the picture track
        // from +0x10; without one, 0x6EC640 gives picture -1). The keys are the picture track's
        // frames, all of which an animation can reach, so each one counts; no other part of an
        // element names a picture.
        std::uint32_t element = 0, block = 0, frames = 0, keys = 0;
        std::int16_t keyCount = 0;
        if (!U32(ems, table + 4ull * i, element)) return false;
        if (!element) continue;
        if (!U32(ems, element + 8ull, block)) return false;
        if (!block) continue;
        if (!S16(ems, block + 4ull, keyCount) || !U32(ems, block + 0x10ull, frames) || !U32(ems, block + 0x14ull, keys)) return false;
        if (keyCount <= 0 || !frames || !keys) continue;
        if (keyCount > static_cast<std::int32_t>(MostKeys) || !Fits(ems, keys, 32ull * static_cast<std::uint32_t>(keyCount))) return false;
        for (std::int32_t k = 0; k < keyCount; ++k) {
            std::uint32_t picture = 0;
            if (!U32(ems, keys + 32ull * static_cast<std::uint32_t>(k), picture)) return false;
            // The game skips a negative number.
            if (static_cast<std::int32_t>(picture) > highest) highest = static_cast<std::int32_t>(picture);
        }
    }
    return true;
}

namespace {
struct Pair { fs::path layout, sheet; };

bool Read(const fs::path& path, Bytes& bytes) {
    const auto read = durable::ReadBounded(path, MostFile);
    if (read.status != durable::ReadStatus::Read) return false;
    bytes = read.bytes;
    return true;
}
std::wstring Lower(std::wstring text) {
    for (auto& c : text) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return text;
}
bool EndsWith(const std::wstring& lowered, const wchar_t* suffix) {
    return lowered.size() >= SuffixSize && lowered.compare(lowered.size() - SuffixSize, SuffixSize, suffix) == 0;
}
}

game_themes::Verdict Check(const std::wstring& themeHudFolder, const GameFile& gameFile, std::string& reason) {
    // The pairs the theme has a half of, by their folder and name under battle\hud.
    std::map<std::wstring, Pair> pairs;
    std::map<std::wstring, std::wstring> shown;
    std::error_code failed;
    const fs::path root(themeHudFolder);
    for (fs::recursive_directory_iterator it(root, failed), end; !failed && it != end; it.increment(failed)) {
        const std::wstring file = it->path().filename().wstring(), lowered = Lower(file);
        const bool layout = EndsWith(lowered, Layout), sheet = EndsWith(lowered, Sheet);
        if ((!layout && !sheet) || !it->is_regular_file(failed)) continue;
        std::wstring stem = it->path().lexically_relative(root).generic_wstring();
        stem.resize(stem.size() - SuffixSize);
        const std::wstring key = Lower(stem);
        if (pairs.size() >= MostPairs && !pairs.count(key)) { reason = "more than " + std::to_string(MostPairs) + " HUD layouts and sheets"; return Verdict::Unreadable; }
        (layout ? pairs[key].layout : pairs[key].sheet) = it->path();
        shown[key] = stem;
    }
    if (failed) { reason = failed.message(); return Verdict::Unreadable; }
    for (auto& [key, pair] : pairs) {
        const std::wstring& stem = shown[key];
        if (pair.layout.empty()) pair.layout = gameFile(stem + Layout);
        if (pair.sheet.empty()) pair.sheet = gameFile(stem + Sheet);
        // Every layout is in CMN; a sheet in a language folder (ENG/cockpit_text) or a fighter's
        // (ENG/RYU/chara) is drawn with the CMN layout of its name. Not uc: the game's own CMN/uc layout
        // has a key for pictures 2 and 3 that every fighter's two-picture uc sheet lacks.
        const auto slash = key.rfind(L'/');
        if (pair.layout.empty() && slash != std::wstring::npos && key.compare(0, 4, L"cmn/") != 0 && key.substr(slash + 1) != L"uc") {
            const std::wstring base = stem.substr(slash + 1);
            const auto own = pairs.find(L"cmn/" + key.substr(slash + 1));
            pair.layout = own != pairs.end() && !own->second.layout.empty() ? own->second.layout : gameFile(L"CMN/" + base + Layout);
        }
        // A layout or a sheet that nothing has is not a pair the game draws.
        if (pair.layout.empty() || pair.sheet.empty()) continue;
        const std::string name = WideToUtf8(stem);
        Bytes layoutBytes, sheetBytes;
        std::uint32_t pictures = 0;
        std::int64_t highest = -1;
        if (!Read(pair.layout, layoutBytes) || !LayoutHighestPicture(layoutBytes, highest)) { reason = name + ": the layout cannot be read"; return Verdict::Unreadable; }
        if (!Read(pair.sheet, sheetBytes) || !SheetPictures(sheetBytes, pictures)) { reason = name + ": the picture sheet cannot be read"; return Verdict::Unreadable; }
        if (highest >= static_cast<std::int64_t>(pictures)) {
            reason = name + ": the layout uses picture " + std::to_string(highest) + " and the sheet has " + std::to_string(pictures);
            return Verdict::PicturesMismatch;
        }
    }
    return Verdict::Accepted;
}
} } }
