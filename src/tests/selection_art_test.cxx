// SelectionArt loading with a real D3D9 device: the portrait fallback, retry
// of a file that fails to decode, and which failures are final and logged.
#include "../ui/SelectionArt.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"
#include "temp_root.hxx"
#include "test_support.hxx"

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
// A Ranking.emz like the game's: one EMB entry `name` holding a plain 32-bit
// BGRA DDS of `side` pixels, which Windows' DDS codec cannot read. The EMZ
// holds it in one stored deflate block.
std::string RankingArchive(const std::string& name, std::uint32_t side) {
    std::string dds(128 + side * side * 4, '\x7f');
    std::memcpy(&dds[0], "DDS ", 4);
    Put32(dds, 4, 124); Put32(dds, 8, 0x81007); Put32(dds, 12, side); Put32(dds, 16, side); Put32(dds, 20, side * side * 4);
    std::memset(&dds[24], 0, 104);
    Put32(dds, 76, 32); Put32(dds, 80, 0x41); Put32(dds, 88, 32);
    Put32(dds, 92, 0xff0000); Put32(dds, 96, 0xff00); Put32(dds, 100, 0xff); Put32(dds, 104, 0xff000000); Put32(dds, 108, 0x1000);
    std::string emb(64, '\0');
    std::memcpy(&emb[0], "#EMB", 4);
    Put32(emb, 12, 1); Put32(emb, 24, 32); Put32(emb, 28, 40);
    Put32(emb, 32, 64 - 32); Put32(emb, 36, static_cast<std::uint32_t>(dds.size()));
    Put32(emb, 40, 44); std::memcpy(&emb[44], name.c_str(), name.size() + 1);
    emb += dds;
    const auto length = static_cast<std::uint16_t>(emb.size());
    std::string emz(16, '\0');
    std::memcpy(&emz[0], "#EMZ", 4);
    Put32(emz, 8, static_cast<std::uint32_t>(emb.size())); Put32(emz, 12, 16);
    emz += '\x01';
    emz.append(reinterpret_cast<const char*>(&length), 2);
    const std::uint16_t inverse = static_cast<std::uint16_t>(~length);
    emz.append(reinterpret_cast<const char*>(&inverse), 2);
    return emz + emb;
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

        // The shipped developer mark sits in the input art folder beside the
        // selection art and loads at its own, wide shape.
        std::ifstream markFile(fs::path(SF4E_TEST_ASSET_ROOT) / L".." / L"input" / L"mark_developer.png", std::ios::binary);
        const std::string mark((std::istreambuf_iterator<char>(markFile)), std::istreambuf_iterator<char>());
        CHECK(mark.size() > 8);
        Write(root / L"input" / L"mark_developer.png", mark);
        const auto developer = Settle(art, [&] { return art.DeveloperMark(); }, 2000);
        CHECK(developer.texture && !developer.missing && developer.width == 256 && developer.height == 116);
        CHECK(logged.size() == 4);
    }
    device->Release(); d3d->Release(); DestroyWindow(window);
    RemoveTempRoot(root);
    std::puts("SelectionArt fallback, retry and failure reporting passed");
    return 0;
}
