// SelectionArt loading with a real D3D9 device: the portrait fallback, retry
// of a file that fails to decode, and which failures are final and logged.
#include "../ui/SelectionArt.hxx"
#include "../common/FighterCatalog.hxx"
#include "temp_root.hxx"
#include "test_support.hxx"

#include <windows.h>
#include <d3d9.h>
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
    SelectionArt::SetLogger([&](const std::string& line) { logged.push_back(line); });
    {
        SelectionArt art(device, game.wstring(), assets.wstring());

        // No game portrait: the packaged original-outfit cutout stands in.
        Write(assets / L"RYU" / L"costume-0" / L"color-0-cutout.png", png);
        const int ryu = FighterId("RYU");
        const auto portrait = Settle(art, [&] { return art.Portrait(ryu, true); }, 2000);
        CHECK(portrait.texture && !portrait.missing && portrait.width > 0);

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
    }
    SelectionArt::SetLogger(nullptr);
    device->Release(); d3d->Release(); DestroyWindow(window);
    RemoveTempRoot(root);
    std::puts("SelectionArt fallback, retry and failure reporting passed");
    return 0;
}
