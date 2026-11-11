// SelectionArt loading with a real D3D9 device: the portrait fallback, retry
// of a file that fails to decode, and which failures are final and logged.
#include "../ui/SelectionArt.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/PointDigits.hxx"
#include "../common/StageCatalog.hxx"
#include "temp_root.hxx"
#include "test_support.hxx"

#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using sf4e::ui::SelectionArt;
using sf4e::ui::SelectionImage;

namespace {
int FighterId(const char* code) {
    for (int id = 0; id < sf4e::selection::FighterCount; ++id)
        if (std::strcmp(sf4e::selection::FindFighter(id)->code, code) == 0) return id;
    return -1;
}
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
void Put32(std::string& bytes, std::size_t at, std::uint32_t value) { std::memcpy(&bytes[at], &value, 4); }
// A plain 32-bit BGRA DDS of `side` pixels, which Windows' DDS codec cannot
// read, like the game's class emblems. The `margin` pixels round its edge are
// transparent, as a crown's canvas is round the crown.
std::string PlainDds(std::uint32_t side, std::uint32_t margin = 0) {
    std::string dds(128 + side * side * 4, '\x7f');
    for (std::uint32_t y = 0; y < side; ++y) for (std::uint32_t x = 0; x < side; ++x)
        if (x < margin || y < margin || x >= side - margin || y >= side - margin) std::memset(&dds[128 + (y * side + x) * 4], 0, 4);
    std::memcpy(&dds[0], "DDS ", 4);
    Put32(dds, 4, 124); Put32(dds, 8, 0x81007); Put32(dds, 12, side); Put32(dds, 16, side); Put32(dds, 20, side * side * 4);
    std::memset(&dds[24], 0, 104);
    Put32(dds, 76, 32); Put32(dds, 80, 0x41); Put32(dds, 88, 32);
    Put32(dds, 92, 0xff0000); Put32(dds, 96, 0xff00); Put32(dds, 100, 0xff); Put32(dds, 104, 0xff000000); Put32(dds, 108, 0x1000);
    return dds;
}
// An EMZ like the game's: one EMB entry `name` holding `dds`, in stored
// deflate blocks (a stored block holds at most 65535 bytes, so a 256x256
// sheet takes several).
std::string Archive(const std::string& name, const std::string& dds) {
    std::string emb(64, '\0');
    std::memcpy(&emb[0], "#EMB", 4);
    Put32(emb, 12, 1); Put32(emb, 24, 32); Put32(emb, 28, 40);
    Put32(emb, 32, 64 - 32); Put32(emb, 36, static_cast<std::uint32_t>(dds.size()));
    Put32(emb, 40, 44); std::memcpy(&emb[44], name.c_str(), name.size() + 1);
    emb += dds;
    std::string emz(16, '\0');
    std::memcpy(&emz[0], "#EMZ", 4);
    Put32(emz, 8, static_cast<std::uint32_t>(emb.size())); Put32(emz, 12, 16);
    for (std::size_t at = 0; at < emb.size();) {
        const auto length = static_cast<std::uint16_t>((std::min)(emb.size() - at, std::size_t{65535}));
        emz += at + length == emb.size() ? '\x01' : '\x00';
        emz.append(reinterpret_cast<const char*>(&length), 2);
        const std::uint16_t inverse = static_cast<std::uint16_t>(~length);
        emz.append(reinterpret_cast<const char*>(&inverse), 2);
        emz.append(emb, at, length);
        at += length;
    }
    return emz;
}
std::string RankingArchive(const std::string& name, std::uint32_t side, std::uint32_t margin = 0) { return Archive(name, PlainDds(side, margin)); }
// The point digits' sheet as the game draws it (PointDigits.hxx): glyph blocks
// of alpha in the places the real sheet has them, `drop` of a band's glyphs
// left out, and the first digit of the white set `taller` pixels taller.
std::string DigitSheet(int drop = 0, int taller = 0) {
    std::string dds = PlainDds(256);
    std::memset(&dds[128], 0, 256 * 256 * 4);
    struct Row { int top, bottom; std::vector<std::pair<int, int>> columns; };
    const Row rows[] = {
        {3, 29, {{3, 24}, {33, 52}, {63, 75}, {90, 107}, {118, 135}, {145, 164}}}, {37, 61, {{34, 52}, {61, 80}, {89, 107}, {117, 136}, {145, 164}}},
        {72, 97, {{3, 24}, {33, 52}, {63, 75}, {90, 107}, {118, 135}, {145, 164}}}, {105, 129, {{34, 52}, {61, 80}, {89, 107}, {117, 136}, {145, 164}}},
        {143, 173, {{9, 31}, {52, 66}, {90, 111}, {130, 151}, {169, 192}, {210, 232}}},
        {183, 213, {{9, 31}, {50, 71}, {89, 112}, {129, 151}, {178, 187}, {206, 238}}}};
    for (const auto& row : rows) {
        const std::size_t keep = row.columns.size() - (&row == &rows[5] ? drop : 0);
        for (std::size_t i = 0; i < keep; ++i)
            for (int y = row.top - (&row == &rows[4] && i == 0 ? taller : 0); y <= row.bottom; ++y)
                for (int x = row.columns[i].first; x <= row.columns[i].second; ++x) dds[128 + (y * 256 + x) * 4 + 3] = '\xff';
    }
    return dds;
}
// Pumps until `request` has settled or `frames` have passed.
template <class Request> SelectionImage Settle(SelectionArt& art, Request request, int frames) {
    SelectionImage image;
    for (int frame = 0; frame < frames; ++frame) {
        art.Pump();
        image = request();
        if (image.texture || image.missing) return image;
        Sleep(1);
    }
    return image;
}
}

int main() {
    HWND window = CreateWindowA("STATIC", "SelectionArt test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    CHECK(window && d3d);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE; parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8; parameters.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    CHECK(d3d && SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device)));
    if (!device) return 1;

    const fs::path root = MakeTempRoot(L"sf4e-selection-art-");
    const fs::path game = root / L"game", assets = root / L"assets";
    fs::create_directories(game);
    const fs::path source = fs::path(SF4E_TEST_ASSET_ROOT) / L"RYU" / L"costume-0" / L"color-0-cutout.png";
    std::ifstream sourceFile(source, std::ios::binary);
    const std::string png((std::istreambuf_iterator<char>(sourceFile)), std::istreambuf_iterator<char>());
    CHECK(png.size() > 8);
    std::vector<std::string> logged;
    {
        SelectionArt art(device, game.wstring(), assets.wstring(), [&](const std::string& line) { logged.push_back(line); });

        // No game portrait: the packaged original-outfit cutout stands in.
        Write(assets / L"RYU" / L"costume-0" / L"color-0-cutout.png", png);
        const int ryu = FighterId("RYU");
        const auto portrait = Settle(art, [&] { return art.Portrait(ryu, true); }, 2000);
        CHECK(portrait.texture && !portrait.missing && portrait.width > 0);
        // A small drawn portrait uses the thumbnail, a large one the full image.
        const auto thumbnail = Settle(art, [&] { return art.PortraitFor(ryu, 48.f); }, 2000);
        CHECK(thumbnail.texture && thumbnail.texture != portrait.texture &&
            thumbnail.width <= SelectionArt::ThumbnailSide && thumbnail.height <= SelectionArt::ThumbnailSide);
        CHECK(art.PortraitFor(ryu, 200.f).texture == portrait.texture);

        // Nothing anywhere: final at once, and a missing portrait is logged.
        const int ken = FighterId("KEN");
        const auto none = Settle(art, [&] { return art.Portrait(ken, true); }, 2000);
        CHECK(none.missing && !none.texture);
        CHECK(logged.size() == 1 && logged[0].find("KEN/portrait-large") != std::string::npos &&
            logged[0].find("no game or package file") != std::string::npos);

        // An absent color preview is expected: final and not logged.
        const auto preview = Settle(art, [&] { return art.Appearance(ken, 0, 3); }, 2000);
        CHECK(preview.missing);
        CHECK(logged.size() == 1);

        // A file that fails to decode is retried rather than blanked for the
        // session; once the file is readable the art appears.
        const fs::path ultra = assets / L"KEN" / L"ultra-0.png";
        Write(ultra, "not a png");
        auto image = art.Ultra(ken, 0);
        for (int frame = 0; frame < 30; ++frame) { art.Pump(); image = art.Ultra(ken, 0); Sleep(1); }
        CHECK(!image.missing && !image.texture);
        Write(ultra, png);
        image = Settle(art, [&] { return art.Ultra(ken, 0); }, 2000);
        CHECK(image.texture && !image.missing);
        CHECK(logged.size() == 1);

        // A file that never decodes is reported missing after its attempts,
        // with the reason, once.
        Write(assets / L"KEN" / L"ultra-1.png", "still not a png");
        image = Settle(art, [&] { return art.Ultra(ken, 1); }, 4000);
        CHECK(image.missing && !image.texture);
        CHECK(logged.size() == 2 && logged[1].find("KEN/ultra-1") != std::string::npos &&
            logged[1].find("3 attempts") != std::string::npos && logged[1].find("decode") != std::string::npos);

        // A game portrait that exists but fails still shows the packaged
        // cutout, and the game file's failure is reported once.
        Write(game / L"resource" / L"ui" / L"chara_select" / L"chara" / L"sel_KEN.tex.emz", "not an archive");
        Write(assets / L"KEN" / L"costume-0" / L"color-0-cutout.png", png);
        const auto fallback = Settle(art, [&] { return art.Portrait(ken, false); }, 2000);
        CHECK(fallback.texture && !fallback.missing);
        CHECK(logged.size() == 3 && logged[2].find("KEN/portrait-thumb uses a fallback") != std::string::npos &&
            logged[2].find("sel_KEN.tex.emz") != std::string::npos);

        // The Random stage card reads only the game's random tile; without it
        // the card is final at once and reported once, like a missing portrait.
        const auto random = Settle(art, [&] { return art.Stage(sf4e::selection::RandomStageId); }, 2000);
        CHECK(random.missing && !random.texture);
        CHECK(logged.size() == 4 && logged[3].find("stages/random") != std::string::npos &&
            logged[3].find("no game or package file") != std::string::npos);
        CHECK(art.Stage(sf4e::selection::RandomStageId).missing && logged.size() == 4);

        // The game's class emblems are plain 32-bit DDS images inside
        // Ranking.emz; they load at their own size without a log line.
        Write(game / L"resource" / L"ui" / L"network_mode" / L"Ranking.emz", RankingArchive("class_B.dds", 32));
        const auto emblem = Settle(art, [&] { return art.RankingEmblem("class_B.dds"); }, 2000);
        CHECK(emblem.texture && !emblem.missing && emblem.width == 32 && emblem.height == 32);
        CHECK(logged.size() == 4);
        // An emblem painted only inside a transparent margin (a crown on its
        // canvas) is shown by its painting: the uv rectangle leaves the margin
        // out, two pixels of it kept, as portraits are cropped, and the size
        // reported is the painted 20 pixels, not the 32-pixel canvas.
        Write(game / L"resource" / L"ui" / L"network_mode" / L"Ranking.emz", RankingArchive("class_MASTER.dds", 32, 8));
        const auto crown = Settle(art, [&] { return art.RankingEmblem("class_MASTER.dds"); }, 2000);
        CHECK(crown.texture && crown.width == 20 && crown.height == 20);
        CHECK(std::abs(crown.uvMin.x - 6.f / 32) < .001f && std::abs(crown.uvMin.y - 6.f / 32) < .001f);
        CHECK(std::abs(crown.uvMax.x - 26.f / 32) < .001f && std::abs(crown.uvMax.y - 26.f / 32) < .001f);
        CHECK(logged.size() == 4);

        // The shipped developer mark sits in the input art folder beside the
        // selection art and loads at its own, wide shape.
        std::ifstream markFile(fs::path(SF4E_TEST_ASSET_ROOT) / L".." / L"input" / L"mark_developer.png", std::ios::binary);
        const std::string mark((std::istreambuf_iterator<char>(markFile)), std::istreambuf_iterator<char>());
        CHECK(mark.size() > 8);
        Write(root / L"input" / L"mark_developer.png", mark);
        const auto developer = Settle(art, [&] { return art.DeveloperMark(); }, 2000);
        CHECK(developer.texture && !developer.missing && developer.width == 256 && developer.height == 116);
        CHECK(logged.size() == 4);

        // The game's Ultra marks come from its character select archives. The
        // W is read only from Ultra's own archive: in an older one it is not
        // looked for, and without it the mark is missing at once and quietly,
        // as the menus draw their own badge then.
        const fs::path select = game / L"resource" / L"ui" / L"chara_select";
        Write(select / L"chara_select.emz", RankingArchive("uc3.dds", 32));
        const auto noW = Settle(art, [&] { return art.UltraIcon(2); }, 2000);
        CHECK(noW.missing && !noW.texture);
        CHECK(logged.size() == 4);
        // Ultra's own archive gives the I at its own size, cropped to its painting.
        Write(game / L"dlc" / L"04_ae2" / L"ui" / L"chara_select" / L"chara_select_dlc4.emz", RankingArchive("uc1.dds", 32, 4));
        const auto one = Settle(art, [&] { return art.UltraIcon(0); }, 2000);
        CHECK(one.texture && !one.missing && one.width == 28 && one.height == 28);
        CHECK(logged.size() == 4);
        // An older archive stands in for a mark Ultra's lacks, and says so once.
        Write(select / L"chara_select.emz", RankingArchive("uc2.dds", 32));
        const auto two = Settle(art, [&] { return art.UltraIcon(1); }, 2000);
        CHECK(two.texture && !two.missing && two.width == 32);
        CHECK(logged.size() == 5 && logged[4].find("ultra-icons/uc2.dds uses a fallback") != std::string::npos);
        CHECK(!art.UltraIcon(3).texture && !art.UltraIcon(-1).texture);

        // The game's point digits come from its ranked result archive: the
        // sheet loads at its own size, and each glyph's ink is measured from
        // its alpha (the digit 7 of the white set is the second row's second).
        const fs::path result = game / L"resource" / L"ui" / L"net_result" / L"CMN" / L"net_result.tex.emz";
        Write(result, Archive("net_vs_number.dds", DigitSheet()));
        const auto points = Settle(art, [&] { return art.PointDigits().image; }, 2000);
        const auto sheet = art.PointDigits().sheet;
        if (!(points.texture && !points.missing && sheet))
            for (const auto& line : logged) std::printf("art said: %s\n", line.c_str());
        CHECK(points.texture && !points.missing && points.width == 256 && points.height == 256 && sheet);
        if (sheet) {
            const auto& seven = sheet->sets[sf4e::point_digits::White][7];
            CHECK(seven.x == 50 && seven.w == 22 && seven.y == 183 && seven.h == 31);
            const auto& gold = sheet->sets[sf4e::point_digits::Gold];
            CHECK(gold[0].x == 33 && gold[0].w == 20 && gold[0].y == 3 && gold[0].h == 27);
            CHECK(gold[9].x == 145 && gold[9].y == 37 && gold[9].h == 25);
            CHECK(sheet->sets[sf4e::point_digits::Blue][5].y == 105 && sheet->sets[sf4e::point_digits::Blue][4].x == 145);
        }
        CHECK(logged.size() == 5);
    }
    {
        // An Ultra mark far larger than the game's 32-pixel icons is refused
        // before it is decoded, as an unusable file: retried, then reported once.
        std::vector<std::string> lines;
        SelectionArt art(device, game.wstring(), assets.wstring(), [&](const std::string& line) { lines.push_back(line); });
        Write(game / L"dlc" / L"04_ae2" / L"ui" / L"chara_select" / L"chara_select_dlc4.emz", RankingArchive("uc1.dds", 96));
        const auto huge = Settle(art, [&] { return art.UltraIcon(0); }, 4000);
        CHECK(huge.missing && !huge.texture);
        CHECK(lines.size() == 1 && lines[0].find("ultra-icons/uc1.dds") != std::string::npos && lines[0].find("96x96") != std::string::npos);
        // A point digits' sheet of another size, or with a glyph missing from a
        // row, is refused as unusable, retried, then reported once.
        const fs::path result = game / L"resource" / L"ui" / L"net_result" / L"CMN" / L"net_result.tex.emz";
        Write(result, Archive("net_vs_number.dds", PlainDds(128)));
        const auto undersized = Settle(art, [&] { return art.PointDigits().image; }, 4000);
        CHECK(undersized.missing && !undersized.texture && !art.PointDigits().sheet);
        CHECK(lines.size() == 2 && lines[1].find("point-digits") != std::string::npos && lines[1].find("not 256x256") != std::string::npos);
        sf4e::point_digits::Sheet measured;
        std::string why;
        const std::string whole = DigitSheet(), gapped = DigitSheet(1);
        const auto bytes = [](const std::string& dds) { return reinterpret_cast<const unsigned char*>(dds.data()) + 128; };
        CHECK(sf4e::point_digits::Measure(bytes(whole), 256, 256, measured, why));
        CHECK(!sf4e::point_digits::Measure(bytes(gapped), 256, 256, measured, why) && why.find("row 183 holds 5 glyphs, not 6") != std::string::npos);
        // Glyphs may be taller than the nominal band, up to its slack.
        const std::string tall = DigitSheet(0, 2);
        CHECK(sf4e::point_digits::Measure(bytes(tall), 256, 256, measured, why) && measured.sets[sf4e::point_digits::White][0].h == 33);
        CHECK(!sf4e::point_digits::Measure(nullptr, 256, 256, measured, why));
    }
    device->Release(); d3d->Release(); DestroyWindow(window);
    RemoveTempRoot(root);
    std::puts("SelectionArt fallback, retry and failure reporting passed");
    return 0;
}
