#include "../platform/HudBundleCheck.hxx"
#include "test_support.hxx"
#include <zlib.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
namespace bundle = sf4e::platform::hud_bundle;
using sf4e::platform::game_themes::Verdict;
using Bytes = std::vector<std::uint8_t>;

namespace {
void PutU32(Bytes& d, size_t at, std::uint32_t v) {
    if (d.size() < at + 4) d.resize(at + 4);
    std::memcpy(d.data() + at, &v, 4);
}
void PutS16(Bytes& d, size_t at, std::int16_t v) {
    if (d.size() < at + 2) d.resize(at + 2);
    std::memcpy(d.data() + at, &v, 2);
}
void Magic(Bytes& d, const char* magic) { std::memcpy(d.data(), magic, 4); d[4] = 0xFE; d[5] = 0xFF; }

// A layout whose elements each use the given picture numbers (an element with none has no picture block).
Bytes Ems(const std::vector<std::vector<std::int32_t>>& elements) {
    Bytes d(0x20 + 4 * elements.size(), 0);
    Magic(d, "#EMS");
    PutU32(d, 0x18, static_cast<std::uint32_t>(elements.size()));
    PutU32(d, 0x1C, 0x20);
    for (size_t i = 0; i < elements.size(); ++i) {
        const size_t element = d.size();
        d.resize(element + 0x40);
        PutU32(d, 0x20 + 4 * i, static_cast<std::uint32_t>(element));
        if (elements[i].empty()) continue;
        // As the game's files have it: the block's key frames (4 bytes each) at +0x10, its keys at +0x14,
        // both offsets from the start of the file.
        const size_t block = d.size();
        d.resize(block + 0x20);
        const size_t frames = d.size();
        d.resize(frames + 4 * elements[i].size());
        const size_t keys = d.size();
        d.resize(keys + 32 * elements[i].size());
        PutU32(d, element + 8, static_cast<std::uint32_t>(block));
        PutS16(d, block + 4, static_cast<std::int16_t>(elements[i].size()));
        PutU32(d, block + 0x10, static_cast<std::uint32_t>(frames));
        PutU32(d, block + 0x14, static_cast<std::uint32_t>(keys));
        for (size_t k = 0; k < elements[i].size(); ++k) PutU32(d, keys + 32 * k, static_cast<std::uint32_t>(elements[i][k]));
    }
    return d;
}
// A layout with one element using pictures 0 to highest.
Bytes Layout(int highest) {
    std::vector<std::int32_t> used;
    for (int i = 0; i <= highest; ++i) used.push_back(i);
    return Ems({ used });
}
// A sheet of `pictures` entries.
Bytes Sheet(std::uint32_t pictures) {
    Bytes d(0x20 + 8 * pictures + 4 * pictures, 0);
    Magic(d, "#EMB");
    PutU32(d, 0xC, pictures);
    PutU32(d, 0x18, 0x20);
    for (std::uint32_t i = 0; i < pictures; ++i) { PutU32(d, 0x20 + 8 * i, 8 * pictures - 4 * i); PutU32(d, 0x24 + 8 * i, 4); }
    return d;
}
// The #EMZ wrapper: size at +8, the deflate stream at +0x10.
Bytes Wrap(const Bytes& body) {
    z_stream z{};
    CHECK(deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    Bytes out(16 + deflateBound(&z, static_cast<uLong>(body.size())));
    z.next_in = const_cast<Bytef*>(body.data()); z.avail_in = static_cast<uInt>(body.size());
    z.next_out = out.data() + 16; z.avail_out = static_cast<uInt>(out.size() - 16);
    CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END);
    out.resize(16 + z.total_out);
    deflateEnd(&z);
    std::memcpy(out.data(), "#EMZ", 4);
    PutU32(out, 8, static_cast<std::uint32_t>(body.size()));
    PutU32(out, 12, 16);
    return out;
}

struct Folders {
    fs::path root;
    Folders() {
        wchar_t temp[MAX_PATH], unique[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, temp));
        CHECK(GetTempFileNameW(temp, L"s4h", 0, unique));
        CHECK(DeleteFileW(unique));
        root = unique;
        CHECK(fs::create_directory(root));
    }
    ~Folders() { std::error_code ignored; fs::remove_all(root, ignored); }
    void Put(const char* side, const char* name, const Bytes& bytes) {
        const fs::path path = root / side / name;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        CHECK(out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    }
    void Clear() { std::error_code ignored; fs::remove_all(root / "theme", ignored); fs::remove_all(root / "game", ignored); }
    Verdict Check(std::string& reason) {
        return bundle::Check((root / "theme").wstring(), [&](const std::wstring& relative) {
            const fs::path path = root / "game" / relative;
            std::error_code ignored;
            return fs::is_regular_file(path, ignored) ? path : fs::path();
        }, reason);
    }
};

void Reading() {
    std::uint32_t count = 0;
    std::int64_t highest = 0;
    CHECK(bundle::SheetPictures(Sheet(9), count) && count == 9);
    CHECK(bundle::SheetPictures(Wrap(Sheet(15)), count) && count == 15);
    CHECK(bundle::SheetPictures(Sheet(0), count) && count == 0);
    CHECK(bundle::LayoutHighestPicture(Layout(8), highest) && highest == 8);
    CHECK(bundle::LayoutHighestPicture(Wrap(Layout(14)), highest) && highest == 14);
    CHECK(bundle::LayoutHighestPicture(Ems({}), highest) && highest == -1);
    CHECK(bundle::LayoutHighestPicture(Ems({ {}, { -1 }, { 3, 1 } }), highest) && highest == 3);
    // A picture block without key frames is not played: its keys name no picture the game reads.
    {
        Bytes framesless = Layout(20);
        std::uint32_t element = 0, block = 0;
        std::memcpy(&element, framesless.data() + 0x20, 4);
        std::memcpy(&block, framesless.data() + element + 8, 4);
        PutU32(framesless, block + 0x10, 0);
        CHECK(bundle::LayoutHighestPicture(framesless, highest) && highest == -1);
    }

    // Not the file asked for, cut short, or with offsets outside it: refused, never read past the end.
    CHECK(!bundle::LayoutHighestPicture(Sheet(2), highest) && !bundle::SheetPictures(Layout(2), count));
    CHECK(!bundle::LayoutHighestPicture({}, highest) && !bundle::SheetPictures({}, count));
    for (const Bytes& whole : { Layout(5), Wrap(Layout(5)), Ems({ {}, { 1, 2 } }) })
        for (size_t keep = 0; keep < whole.size(); keep += 7) {
            const Bytes cut(whole.begin(), whole.begin() + keep);
            std::int64_t any = 0;
            if (bundle::LayoutHighestPicture(cut, any)) CHECK(false);
        }
    for (const Bytes& whole : { Sheet(5), Wrap(Sheet(5)) })
        for (size_t keep = 0; keep < whole.size(); keep += 5) {
            const Bytes cut(whole.begin(), whole.begin() + keep);
            std::uint32_t any = 0;
            if (bundle::SheetPictures(cut, any) && keep < 0x20 + 8 * 5) CHECK(false);
        }
    Bytes d = Layout(3);
    PutU32(d, 0x1C, 0xFFFFFFF0u);                                   // the element table, almost at 4 GB
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    d = Layout(3);
    PutU32(d, 0x20, 0xFFFFFFFFu);                                   // an element
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    d = Layout(3);
    PutU32(d, 0x18, 0xFFFFFFFFu);                                   // an element count
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    d = Layout(3);
    PutU32(d, 0x20, 0xFFFFFFFCu);                                   // an element whose block lies beyond the end
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    // The element's picture block is found through the element table's one slot.
    d = Layout(3);
    std::uint32_t at = 0, block = 0;
    std::memcpy(&at, d.data() + 0x20, 4);
    std::memcpy(&block, d.data() + at + 8, 4);
    PutU32(d, block + 0x14, 0xFFFFFFFFu);                           // the keys pointer
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    d = Layout(3);
    PutS16(d, block + 4, 0x7FFF);                                   // far more keys than the file holds
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    d = Layout(3);
    PutS16(d, block + 4, 5);                                        // more keys than it holds, though few
    CHECK(!bundle::LayoutHighestPicture(d, highest));
    Bytes sheet = Sheet(3);
    PutU32(sheet, 0x18, 0xFFFFFFF8u);
    CHECK(!bundle::SheetPictures(sheet, count));
    sheet = Sheet(3);
    PutU32(sheet, 0xC, 0xFFFFFFFFu);
    CHECK(!bundle::SheetPictures(sheet, count));

    // The wrapper's own fields.
    Bytes wrapped = Wrap(Layout(2));
    PutU32(wrapped, 12, 0xFFFFFFFFu);
    CHECK(!bundle::LayoutHighestPicture(wrapped, highest));
    wrapped = Wrap(Layout(2));
    PutU32(wrapped, 12, 4);
    CHECK(!bundle::LayoutHighestPicture(wrapped, highest));
    wrapped = Wrap(Layout(2));
    PutU32(wrapped, 8, 0xFFFFFFFFu);
    CHECK(!bundle::LayoutHighestPicture(wrapped, highest));
    wrapped = Wrap(Layout(2));
    PutU32(wrapped, 8, static_cast<std::uint32_t>(Layout(2).size()) + 1);   // says more than the stream holds
    CHECK(!bundle::LayoutHighestPicture(wrapped, highest));
    wrapped = Wrap(Layout(2));
    wrapped[wrapped.size() / 2] ^= 0xFF;                            // damaged stream
    wrapped[wrapped.size() / 2 + 1] ^= 0xFF;
    std::int64_t damaged = 0;
    if (bundle::LayoutHighestPicture(wrapped, damaged)) CHECK(damaged >= -1);   // inflated to something: still read within bounds

    // A stream that inflates to far more than it says: stopped at what it says.
    const Bytes zeros(48u << 20, 0);
    Bytes bomb = Wrap(zeros);
    CHECK(bomb.size() < (1u << 20));
    PutU32(bomb, 8, 64);
    CHECK(!bundle::LayoutHighestPicture(bomb, highest) && !bundle::SheetPictures(bomb, count));
    PutU32(bomb, 8, 40u << 20);                                      // over the most a HUD file may hold
    CHECK(!bundle::LayoutHighestPicture(bomb, highest));
}

void Pairs() {
    Folders f;
    std::string reason;
    // The theme's own layout and sheet: indices below the count are fine, an index equal to it is not.
    f.Put("theme", "CMN/cockpit.ems.emz", Wrap(Layout(8)));
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(9)));
    CHECK(f.Check(reason) == Verdict::Accepted);
    f.Put("theme", "CMN/cockpit.ems.emz", Layout(9));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch && reason.find("cockpit") != std::string::npos);
    f.Put("theme", "CMN/cockpit.tex.emz", Sheet(10));                // plain files, as a modding tool may write
    CHECK(f.Check(reason) == Verdict::Accepted);

    // A theme with an older 9 picture sheet and no layout: the game's Ultra layout (0 to 14) is the one drawn.
    f.Clear();
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(9)));
    f.Put("game", "CMN/cockpit.ems.emz", Wrap(Layout(14)));
    f.Put("game", "CMN/cockpit.tex.emz", Wrap(Sheet(15)));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch);
    // The same theme with the Ultra sheet, or with a layout of its own for 9 pictures, is fine.
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(15)));
    CHECK(f.Check(reason) == Verdict::Accepted);
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(9)));
    f.Put("theme", "CMN/cockpit.ems.emz", Wrap(Layout(8)));
    CHECK(f.Check(reason) == Verdict::Accepted);

    // A theme with a layout only: it is drawn from the game's sheet.
    f.Clear();
    f.Put("theme", "CMN/cockpit.ems.emz", Wrap(Layout(14)));
    f.Put("game", "CMN/cockpit.tex.emz", Wrap(Sheet(15)));
    CHECK(f.Check(reason) == Verdict::Accepted);
    f.Put("game", "CMN/cockpit.tex.emz", Wrap(Sheet(9)));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch);

    // A half the game has no other half for is left alone, and so is a file that is neither.
    f.Clear();
    f.Put("theme", "CMN/cockpit.ems.emz", Wrap(Layout(14)));
    f.Put("theme", "CMN/readme.txt", Bytes{ 'x' });
    CHECK(f.Check(reason) == Verdict::Accepted);

    // Folders of fighters and languages pair the same way, whatever the case of the names.
    f.Clear();
    f.Put("theme", "ENG/RYU/Chara.EMS.emz", Wrap(Layout(3)));
    f.Put("theme", "ENG/RYU/chara.tex.emz", Wrap(Sheet(3)));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch && reason.find("RYU") != std::string::npos);
    f.Put("theme", "ENG/RYU/chara.tex.emz", Wrap(Sheet(4)));
    f.Put("theme", "ENG/KEN/chara.tex.emz", Wrap(Sheet(1)));
    f.Put("game", "ENG/KEN/chara.ems.emz", Wrap(Layout(0)));
    CHECK(f.Check(reason) == Verdict::Accepted);
    f.Put("game", "ENG/KEN/chara.ems.emz", Wrap(Layout(1)));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch);

    // A sheet in a language or fighter folder is drawn with the CMN layout of its name.
    f.Clear();
    f.Put("theme", "ENG/cockpit_text.tex.emz", Wrap(Sheet(2)));
    f.Put("game", "CMN/cockpit_text.ems.emz", Wrap(Layout(3)));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch && reason.find("ENG/cockpit_text") != std::string::npos);
    f.Put("theme", "CMN/cockpit_text.ems.emz", Wrap(Layout(1)));       // the theme's own CMN layout comes first
    CHECK(f.Check(reason) == Verdict::Accepted);
    f.Put("theme", "ENG/RYU/chara.tex.emz", Wrap(Sheet(3)));
    f.Put("game", "CMN/chara.ems.emz", Wrap(Layout(3)));
    CHECK(f.Check(reason) == Verdict::PicturesMismatch && reason.find("RYU") != std::string::npos);
    f.Put("theme", "ENG/RYU/chara.tex.emz", Wrap(Sheet(4)));
    CHECK(f.Check(reason) == Verdict::Accepted);
    // Not uc: the game's own uc layout names pictures 2 and 3, which no fighter's uc sheet has.
    f.Put("theme", "ENG/RYU/uc.tex.emz", Wrap(Sheet(2)));
    f.Put("game", "CMN/uc.ems.emz", Wrap(Layout(3)));
    CHECK(f.Check(reason) == Verdict::Accepted);

    // A layout with no elements, and one whose elements have no pictures, use none.
    f.Clear();
    f.Put("theme", "CMN/cockpit.ems.emz", Wrap(Ems({})));
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(0)));
    CHECK(f.Check(reason) == Verdict::Accepted);

    // A file that cannot be read is refused with its own verdict, whichever side it is on.
    f.Clear();
    f.Put("theme", "CMN/cockpit.ems.emz", Bytes{ '#', 'E', 'M', 'Z', 0, 0 });
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(3)));
    CHECK(f.Check(reason) == Verdict::Unreadable && !reason.empty());
    f.Put("theme", "CMN/cockpit.ems.emz", Wrap(Layout(1)));
    f.Put("theme", "CMN/cockpit.tex.emz", Bytes{ 1, 2, 3 });
    CHECK(f.Check(reason) == Verdict::Unreadable);
    f.Clear();
    f.Put("theme", "CMN/cockpit.tex.emz", Wrap(Sheet(3)));
    f.Put("game", "CMN/cockpit.ems.emz", Bytes{ 9, 9, 9 });
    CHECK(f.Check(reason) == Verdict::Unreadable);
    // An empty theme folder has nothing to check.
    f.Clear();
    fs::create_directories(f.root / "theme");
    CHECK(f.Check(reason) == Verdict::Accepted);
}
}

int main() {
    Reading();
    Pairs();
    return 0;
}
