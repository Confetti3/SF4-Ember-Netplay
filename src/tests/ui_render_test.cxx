// Render the real UI into a hidden DX9 surface; no game or network is started.
#include "../ui/ApplicationShell.hxx"
#include "../ui/Theme.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/RecoveryMenu.hxx"
#include "../common/Localization.hxx"
#include "../common/StageCatalog.hxx"
#include <imgui_internal.h>
#include <imgui_impl_dx9.h>
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <chrono>
#include <memory>
#include <random>
#include <thread>

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Renderer {
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    Renderer() {
        window = CreateWindowA("STATIC", "SF4 UI rendering check", WS_OVERLAPPEDWINDOW,
            0, 0, 2560, 1440, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        Require(window != nullptr, "Hidden window creation failed");
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        Require(d3d != nullptr, "Direct3D9 unavailable");
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE; p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.BackBufferFormat = D3DFMT_A8R8G8B8; p.BackBufferWidth = 2560; p.BackBufferHeight = 1440;
        p.hDeviceWindow = window;
        Require(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &device)), "DX9 device creation failed");
    }
    ~Renderer() { if (device) device->Release(); if (d3d) d3d->Release(); if (window) DestroyWindow(window); }
    void Resize(int width, int height) {
        D3DPRESENT_PARAMETERS params{};
        params.Windowed = TRUE; params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.BackBufferFormat = D3DFMT_A8R8G8B8;
        params.BackBufferWidth = width; params.BackBufferHeight = height; params.hDeviceWindow = window;
        // Another window, a display change, a sleeping monitor or a lock can
        // take the device away. Wait until it can be reset, as
        // RecoverySurface does, for up to two minutes.
        HRESULT hr = D3DERR_DEVICELOST;
        for (int wait = 0; wait < 2400 && hr == D3DERR_DEVICELOST; ++wait) {
            if (device->TestCooperativeLevel() == D3DERR_DEVICELOST) { Sleep(50); continue; }
            hr = device->Reset(&params);
            if (hr == D3DERR_DEVICELOST) Sleep(50);
        }
        if (FAILED(hr)) {
            char message[64];
            std::snprintf(message, sizeof(message), "DX9 device reset failed (0x%08lx)", static_cast<unsigned long>(hr));
            throw std::runtime_error(message);
        }
    }
    void Draw() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(45, 47, 49), 1.f, 0);
        Require(SUCCEEDED(device->BeginScene()), "BeginScene failed");
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
    void Capture(const std::string& name, int width, int height) {
        IDirect3DSurface9 *target = nullptr, *copy = nullptr;
        Require(SUCCEEDED(device->GetRenderTarget(0, &target)), "GetRenderTarget failed");
        HRESULT result = device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &copy, nullptr);
        if (SUCCEEDED(result)) result = device->GetRenderTargetData(target, copy);
        target->Release();
        Require(SUCCEEDED(result), "Readback failed");
        D3DLOCKED_RECT pixels{};
        Require(SUCCEEDED(copy->LockRect(&pixels, nullptr, D3DLOCK_READONLY)), "Surface lock failed");
        if(name.find("training-")!=std::string::npos) {
            const auto corner=*reinterpret_cast<const unsigned*>(static_cast<const char*>(pixels.pBits)+5*pixels.Pitch+5*4);
            Require((corner&0xffffff)==0x2d2f31,"Training rendering altered the game outside its panel");
        }
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
        file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
        std::ofstream out(name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        for (int y = 0; y < height; ++y)
            out.write(static_cast<const char*>(pixels.pBits) + y * pixels.Pitch, width * 4);
        copy->UnlockRect(); copy->Release();
        Require(out.good(), "Screenshot write failed");
    }
};
ImGuiWindow* FindWindow(const char* fragment) {
    for (auto* window : GImGui->Windows)
        if (window->Active && std::string(window->Name).find(fragment) != std::string::npos) return window;
    throw std::runtime_error(std::string("Missing UI window: ") + fragment);
}
void Activate(const char* windowFragment, const char* label) {
    ImGui::ActivateItemByID(FindWindow(windowFragment)->GetID(label));
}
void CheckStacks() {
    Require(GImGui->CurrentWindowStack.Size == 1, "Unbalanced window stack");
    Require(GImGui->ColorStack.Size == 0 && GImGui->StyleVarStack.Size == 0 &&
            GImGui->FontStack.Size == 0 && GImGui->DisabledStackSize == 0, "Unbalanced style stack");
    Require(GImGui->CurrentTable == nullptr, "Unbalanced table");
    Require(GImGui->ErrorCountCurrentFrame == 0, "ImGui reported a rendering error");
}
// How the 2 GB of address space this 32-bit process has is used: what is
// committed, what is only reserved, and the largest block still free, which is
// what an atlas or texture allocation needs to find.
void PrintAddressSpace(const char* label) {
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    std::size_t committed[3] = {}, reserved[3] = {}, freeTotal = 0, freeLargest = 0;
    const char* names[3] = {"private", "mapped", "image"};
    MEMORY_BASIC_INFORMATION region{};
    for (auto* address = static_cast<char*>(system.lpMinimumApplicationAddress);
         address < static_cast<char*>(system.lpMaximumApplicationAddress) &&
         VirtualQuery(address, &region, sizeof(region)) == sizeof(region);
         address = static_cast<char*>(region.BaseAddress) + region.RegionSize) {
        if (region.State == MEM_FREE) { freeTotal += region.RegionSize; if (region.RegionSize > freeLargest) freeLargest = region.RegionSize; continue; }
        const int kind = region.Type == MEM_IMAGE ? 2 : region.Type == MEM_MAPPED ? 1 : 0;
        (region.State == MEM_COMMIT ? committed : reserved)[kind] += region.RegionSize;
    }
    std::fprintf(stderr, "ADDRESS %s: free %zu MB (largest %zu MB)", label, freeTotal >> 20, freeLargest >> 20);
    for (int kind = 0; kind < 3; ++kind)
        std::fprintf(stderr, ", %s %zu MB committed + %zu MB reserved", names[kind], committed[kind] >> 20, reserved[kind] >> 20);
    std::fprintf(stderr, "\n");
}
// A crash here would otherwise be a bare SEGFAULT from ctest. Print what
// faulted, the symbolized stack and how much memory the process held, and write
// a minidump beside the test, so an intermittent failure names itself.
LONG WINAPI ReportCrash(EXCEPTION_POINTERS* info) {
    static volatile LONG reporting = 0;
    if (InterlockedExchange(&reporting, 1)) return EXCEPTION_CONTINUE_SEARCH;
    const auto* record = info->ExceptionRecord;
    std::fprintf(stderr, "UI render check crashed: exception 0x%08lx at %p", record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        std::fprintf(stderr, " (%s address %p)", record->ExceptionInformation[0] == 0 ? "read of" : record->ExceptionInformation[0] == 1 ? "write to" : "execute of",
            reinterpret_cast<void*>(record->ExceptionInformation[1]));
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        std::fprintf(stderr, "\n  working set %zu MB, private %zu MB", memory.WorkingSetSize >> 20, memory.PrivateUsage >> 20);
    std::fprintf(stderr, "\n");
    PrintAddressSpace("at crash");
    const HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(process, nullptr, TRUE)) {
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Eip; frame.AddrFrame.Offset = context.Ebp; frame.AddrStack.Offset = context.Esp;
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        for (int depth = 0; depth < 48 && StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, &context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset; ++depth) {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 255;
            DWORD64 displacement = 0; DWORD lineDisplacement = 0;
            IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
            const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;
            const bool located = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != FALSE;
            std::fprintf(stderr, "  #%d 0x%08llx %s+0x%llx", depth, frame.AddrPC.Offset, named ? symbol->Name : "?", displacement);
            if (located) std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
            std::fprintf(stderr, "\n");
        }
    }
    const HANDLE dump = CreateFileA("UiRenderTest-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(process, GetCurrentProcessId(), dump, MiniDumpWithIndirectlyReferencedMemory, &exception, nullptr, nullptr);
        CloseHandle(dump);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
// The atlas is kept as RGBA (the brand mark is painted into it) and the backend
// uploads it as a texture, so a rebuild needs that many bytes in one piece,
// twice over while the old texture lives. The game is a 32-bit process with
// 2 GB of address space and ImGui does not survive a failed allocation: the
// rebuild crashed in ImFontAtlas::GetTexDataAsRGBA32 when no free block was
// left, at 4096x8192 (128 MB) for a Japanese atlas at 300% scale. Nothing may
// bake more than this, whatever the language, scale or player text.
constexpr int AtlasBudgetPixels = 2048 * 4096; // 32 MB as RGBA
void RequireAtlasWithinBudget(const char* what, float dpi) {
    const auto& atlas = *ImGui::GetIO().Fonts;
    if (atlas.TexWidth * atlas.TexHeight <= AtlasBudgetPixels) return;
    char message[160];
    std::snprintf(message, sizeof(message), "%s at %.2fx bakes a %dx%d atlas, over the 2048x4096 budget", what, dpi, atlas.TexWidth, atlas.TexHeight);
    throw std::runtime_error(message);
}
// Check the baked atlas, not just the source fonts' cmaps: reducing its cost
// must keep the active catalog and the language picker in all three sizes.
void RequireAtlasCoverage(sf4e::loc::Locale locale, float dpi) {
    using namespace sf4e;
    std::string text = loc::DisplayText(locale);
    for (int i = 0; i < static_cast<int>(loc::Locale::Count); ++i)
        text += loc::NativeName(static_cast<loc::Locale>(i));
    ImFontGlyphRangesBuilder characters;
    characters.AddText(text.c_str());
    ImVector<ImWchar> ranges;
    characters.BuildRanges(&ranges);
    for (auto* font : ImGui::GetIO().Fonts->Fonts)
        for (int i = 0; i + 1 < ranges.Size && ranges[i]; i += 2)
            for (unsigned cp = ranges[i]; cp <= ranges[i + 1]; ++cp) {
                if (cp < 0x20 || font->FindGlyphNoFallback(static_cast<ImWchar>(cp))) continue;
                char message[160];
                std::snprintf(message, sizeof(message), "%s at %.2fx lacks U+%04X in the %.0fpx font",
                    loc::Tag(locale), dpi, cp, font->FontSize);
                throw std::runtime_error(message);
            }
}
// Drives the atlas rebuild the way play does: between frames, for a changing
// language and DPI, with player text of characters chosen at random from the
// whole basic multilingual plane, some of which no font draws. Every rebuild
// is followed by a frame that draws that text, so a rebuild that leaves the
// context or the backend holding the old atlas shows up as a crash here, and
// the atlas has to stay within its budget for the fullest text the atlas takes.
void StressAtlas(Renderer& renderer, int iterations, unsigned seed) {
    using namespace sf4e;
    std::mt19937 rng(seed);
    renderer.Resize(1280, 720);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 720); io.DeltaTime = 1.f / 60;
    ui::ApplyTheme(1.f);
    ImGui_ImplDX9_Init(renderer.device);
    const auto appendUtf8 = [](std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    };
    const unsigned edges[] = {0xFFFF, 0xFFFE, 0xD800, 0xDFFF, 0xE000, 0x2028, 0x200B, 0x0300, 0x0080, 0x009F, 0x3000, 0x4E00, 0x9FFF, 0xAC00, 0xD7A3};
    const auto randomText = [&](std::size_t length) {
        std::string text;
        for (std::size_t i = 0; i < length; ++i) {
            const unsigned pick = rng() % 10;
            unsigned cp;
            if (pick < 6) { const unsigned base[] = {0x3040, 0x30A0, 0x4E00, 0xAC00, 0x3400}, span[] = {0x60, 0x60, 0x51A5, 0x2BA4, 0x19C0};
                const unsigned range = rng() % 5; cp = base[range] + rng() % span[range]; }
            else if (pick < 9) cp = 0x80 + rng() % 0xFF7F;
            else cp = edges[rng() % std::size(edges)];
            appendUtf8(text, cp);
        }
        return text;
    };
    const auto drawFrame = [&](const std::string& name, const std::string& chat, const std::string& draft) {
        ImGui_ImplDX9_NewFrame(); ImGui::NewFrame();
        ImGui::Begin("Atlas stress");
        ImGui::PushFont(ui::HeadingFont()); ImGui::TextWrapped("%s", name.c_str()); ImGui::PopFont();
        ImGui::TextWrapped("%s", chat.c_str());
        ImGui::PushFont(ui::DiagnosticFont()); ImGui::TextWrapped("%s", draft.c_str()); ImGui::PopFont();
        ImGui::End();
        ImGui::Render(); renderer.Draw();
    };
    // Every language at every scale the overlay clamps to, first with only its
    // catalog, then holding as many player characters as the atlas takes.
    ui::SetUserGlyphRebuildInterval(std::chrono::milliseconds(0));
    for (const auto locale : {loc::Locale::En, loc::Locale::Ru, loc::Locale::Ja, loc::Locale::Ko, loc::Locale::ZhHans})
        for (const float dpi : {1.f, 1.25f, 1.5f, 2.f, 3.f}) {
            loc::SetActive(locale);
            ui::SetUserGlyphRetention(std::chrono::milliseconds(1)); Sleep(5);
            ui::ApplyTheme(dpi + .01f); ImGui_ImplDX9_InvalidateDeviceObjects(); ui::ApplyTheme(dpi);
            RequireAtlasWithinBudget(loc::Tag(locale), dpi);
            RequireAtlasCoverage(locale, dpi);
            ui::SetUserGlyphRetention(std::chrono::milliseconds(60000));
            std::string full;
            for (unsigned i = 0; i < 512; ++i) appendUtf8(full, i % 3 == 0 ? 0x4E00 + i * 7 : i % 3 == 1 ? 0xAC00 + i * 11 : 0x3400 + i * 5);
            ui::NoteUserText(full);
            Require(ui::ApplyTheme(dpi), "Player text needing 512 glyphs did not rebuild the atlas");
            ImGui_ImplDX9_InvalidateDeviceObjects();
            RequireAtlasWithinBudget((std::string(loc::Tag(locale)) + " with 512 player characters").c_str(), dpi);
            RequireAtlasCoverage(locale, dpi);
            drawFrame("Player", full, "Draft");
        }
    const float scales[] = {1.f, 1.25f, 1.5f, 2.f, 3.f};
    const std::chrono::milliseconds retentions[] = {std::chrono::milliseconds(0), std::chrono::milliseconds(30), std::chrono::milliseconds(10000)};
    int rebuilds = 0;
    for (int frame = 0; frame < iterations; ++frame) {
        loc::SetActive(static_cast<loc::Locale>(rng() % static_cast<unsigned>(loc::Locale::Count)));
        ui::SetUserGlyphRetention(retentions[rng() % std::size(retentions)]);
        const std::string name = randomText(1 + rng() % 24), chat = randomText(rng() % 160), draft = randomText(rng() % 40);
        ui::NoteUserText(name); ui::NoteUserText(chat, ui::UserTextRole::Chat); ui::NoteUserText(draft, ui::UserTextRole::Draft);
        if (ui::ApplyTheme(scales[rng() % std::size(scales)])) { ImGui_ImplDX9_InvalidateDeviceObjects(); ++rebuilds; }
        RequireAtlasWithinBudget("Random player text", ImGui::GetIO().FontGlobalScale);
        drawFrame(name, chat, draft);
        if (rng() % 8 == 0) Sleep(35);
    }
    Require(rebuilds > iterations / 8, "The atlas stress never rebuilt the atlas");
    std::printf("Atlas stress: %d frames, %d atlas rebuilds.\n", iterations, rebuilds);
    ImGui_ImplDX9_Shutdown(); ImGui::DestroyContext();
    loc::SetActive(loc::Locale::En);
}
}

#include "ui_render_public_rooms.hxx"
#include "ui_render_match_hud.hxx"
#include "ui_render_chat.hxx"

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(ReportCrash);
    try {
        Renderer renderer;
        if (const char* stress = std::getenv("SF4E_UI_RENDER_ATLAS_STRESS")) { StressAtlas(renderer, std::atoi(stress), 1); return 0; }
        const std::string output = argc > 1 ? argv[1] : "";
        const bool trainingShotsOnly = argc > 2 && std::string(argv[2]) == "--training-shots-only";
        const bool recoveryShotsOnly = argc > 2 && std::string(argv[2]) == "--recovery-shots-only";
        const bool matchShotsOnly = argc > 2 && std::string(argv[2]) == "--match-shots-only";
        const bool uxShotsOnly = argc > 4 && std::string(argv[4]) == "--ux-shots-only";
        const bool readmeShots = argc > 4 && std::string(argv[4]) == "--readme-shots";
        std::unique_ptr<sf4e::ui::SelectionArt> art;
        if (argc > 2 && !trainingShotsOnly && !recoveryShotsOnly && !matchShotsOnly) {
            const std::string root = argv[2];
            const std::string assets = argc > 3 ? argv[3] : "assets/selection";
            art.reset(new sf4e::ui::SelectionArt(renderer.device, std::wstring(root.begin(), root.end()), std::wstring(assets.begin(), assets.end())));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            int loaded = 0;
            do {
                art->Pump(); loaded = 0;
                for (int id = 0; id < sf4e::selection::FighterCount; ++id) {
                    const auto portrait = art->Portrait(id);
                    if (portrait.missing) throw std::runtime_error(std::string("Native portrait decode failed: ") + sf4e::selection::FindFighter(id)->code);
                    if (portrait.texture) ++loaded;
                }
                if (loaded == sf4e::selection::FighterCount) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < deadline);
            Require(loaded == sf4e::selection::FighterCount, "Native portrait loading timed out");
            std::printf("All 44 native portraits decoded and uploaded to DX9.\n");
            // Exercise the real file decoder/upload path for every costume photo.
            // Also settle these asynchronous loads before taking UI captures.
            const auto costumeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            int expected = 0;
            for (int id = 0; id < sf4e::selection::FighterCount; ++id)
                expected += sf4e::selection::CostumeCount(id) - 1;
            do {
                art->Pump(); loaded = 0;
                for (int id = 0; id < sf4e::selection::FighterCount; ++id)
                    for (int costume = 1; costume < sf4e::selection::CostumeCount(id); ++costume) {
                        const auto photo = art->Appearance(id, costume, 0);
                        if (photo.missing) throw std::runtime_error(std::string("Costume decode failed: ") +
                            sf4e::selection::FindFighter(id)->code + "/" + std::to_string(costume));
                        if (photo.texture) ++loaded;
                    }
                if (loaded == expected) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < costumeDeadline);
            Require(loaded == expected, "Costume photo loading timed out");
            std::printf("All %d alternate costume photos decoded and uploaded to DX9.\n", expected);
            const auto stageDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            do {
                art->Pump(); loaded = 0;
                for (const auto& stage : sf4e::selection::StageList()) {
                    const auto photo = art->Stage(stage.id);
                    if (photo.missing) throw std::runtime_error(std::string("Stage photo decode failed: ") + stage.code);
                    if (photo.texture) {
                        Require(photo.width * 9 == photo.height * 16, "Stage photo aspect ratio changed");
                        ++loaded;
                    }
                }
                if (loaded == sf4e::selection::VersusStageCount) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < stageDeadline);
            Require(loaded == sf4e::selection::VersusStageCount, "Stage photo loading timed out");
            std::printf("All 28 stage photos decoded and uploaded to DX9.\n");
            // The Random card is the game's character-select random tile, cropped
            // to its content, so it is not held to the 16:9 stage framing.
            const auto randomDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            sf4e::ui::SelectionImage randomTile;
            do {
                art->Pump();
                randomTile = art->Stage(sf4e::selection::RandomStageId);
                Require(!randomTile.missing, "Random stage tile decode failed");
                if (randomTile.texture) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < randomDeadline);
            Require(randomTile.texture && randomTile.width > 0 && randomTile.height > 0, "Random stage tile loading timed out");
            std::printf("Random stage tile decoded and uploaded to DX9 (%dx%d).\n", randomTile.width, randomTile.height);
            std::vector<std::pair<int, int>> ultraPhotos;
            for (int id = 0; id < sf4e::selection::FighterCount; ++id) for (int ultra = 0; ultra < 2; ++ultra) {
                const std::string path = assets + "/" + sf4e::selection::FindFighter(id)->code + "/ultra-" + std::to_string(ultra) + ".png";
                const bool present = std::ifstream(path, std::ios::binary).good();
                // The archived 2010 guides cover both Ultras for every SSFIV fighter.
                if (sf4e::selection::EditionAllowed(id, 1, true)) Require(present, "Missing archived SSFIV Ultra photo");
                if (present) ultraPhotos.emplace_back(id, ultra);
            }
            const auto ultraDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            do {
                art->Pump(); loaded = 0;
                for (const auto& option : ultraPhotos) {
                    const auto photo = art->Ultra(option.first, option.second);
                    Require(!photo.missing, "Ultra photo decode failed");
                    if (photo.texture) {
                        Require(photo.width == 256 && photo.height == 144, "Ultra photo framing changed");
                        ++loaded;
                    }
                }
                if (loaded == static_cast<int>(ultraPhotos.size())) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < ultraDeadline);
            Require(loaded == static_cast<int>(ultraPhotos.size()), "Ultra photo loading timed out");
            std::printf("All %d supplied Ultra photos decoded and uploaded to DX9.\n", loaded);
            // Load numbered color assets in bounded groups, allowing the real
            // texture cache to evict previous groups as it does during browsing.
            struct ColorPhoto { int fighter, costume, color; };
            std::vector<ColorPhoto> colorPhotos;
            for (int id = 0; id < sf4e::selection::FighterCount; ++id)
                for (int costume = 0; costume < sf4e::selection::CostumeCount(id); ++costume)
                    for (int color = 0; color < sf4e::selection::ColorCount(id, costume); ++color) {
                        const std::string stem = assets + "/" + sf4e::selection::FindFighter(id)->code +
                            "/costume-" + std::to_string(costume) + "/color-" + std::to_string(color);
                        // Sheet-cropped palettes ship as a cutout alone; a
                        // preserved photograph must always have its cutout.
                        const bool photo = GetFileAttributesA((stem + ".png").c_str()) != INVALID_FILE_ATTRIBUTES;
                        const bool cutout = GetFileAttributesA((stem + "-cutout.png").c_str()) != INVALID_FILE_ATTRIBUTES;
                        Require(!photo || cutout, "Numbered color photograph has not been masked");
                        if (cutout) colorPhotos.push_back({id, costume, color});
                    }
            for (std::size_t first = 0; first < colorPhotos.size(); first += 32) {
                const std::size_t end = (std::min)(first + 32, colorPhotos.size());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                do {
                    art->Pump(); loaded = 0;
                    for (std::size_t i = first; i < end; ++i) {
                        const auto& key = colorPhotos[i];
                        const auto photo = art->Appearance(key.fighter, key.costume, key.color);
                        Require(!photo.missing, "Numbered color decode failed");
                        if (photo.texture) ++loaded;
                    }
                    if (loaded == static_cast<int>(end - first)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } while (std::chrono::steady_clock::now() < deadline);
                Require(loaded == static_cast<int>(end - first), "Numbered color loading timed out");
            }
            std::printf("All %d supplied numbered color cutouts decoded and uploaded to DX9.\n", static_cast<int>(colorPhotos.size()));
            const auto originalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool originalLoaded = false;
            do {
                art->Pump();
                originalLoaded = art->Appearance(0, 0, 0).texture != 0;
                if (originalLoaded) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < originalDeadline);
            Require(originalLoaded, "Original outfit preview failed");
        }
        if(!art)art.reset(new sf4e::ui::SelectionArt(renderer.device,L"",L"" SF4E_TEST_ASSET_ROOT));
        sf4e::ui::SetMenuArt(art.get());
        const auto brandDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!art->MenuBackdrop().texture&&std::chrono::steady_clock::now()<brandDeadline){
            art->Pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Require(art->MenuBackdrop().texture!=0,"Ember background failed to load");
        const char* promptAssets[]={"xbox_button_color_a","xbox_button_color_b","xbox_button_color_x","xbox_button_color_y",
            "xbox_lb","xbox_rb","xbox_lt","xbox_rt","xbox_dpad","xbox_dpad_horizontal","keyboard_enter","keyboard_escape",
            "keyboard_arrows_all","keyboard_arrows_horizontal","generic_button_circle_fill","xbox_button_back","xbox_button_start"};
        for(const auto* prompt:promptAssets){
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!art->InputPrompt(prompt).texture&&std::chrono::steady_clock::now()<deadline){art->Pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            Require(art->InputPrompt(prompt).texture!=0,"Packaged Kenney prompt failed to load");
        }
        struct Size { int w, h; float dpi; };
        // English and the padded pseudo locale sweep every size. The
        // UiRenderLocales run sweeps each translation at the tightest, the
        // commonest and a scaled size, which keeps both runs inside their timeouts.
        const char* localeRun = std::getenv("SF4E_UI_RENDER_LOCALES");
        const bool translations = localeRun && std::string(localeRun) == "translations";
        // SF4E_UI_RENDER_QUICK=1 is for a quick look at a change: English at 1920x1080 only.
        const char* quickRun = std::getenv("SF4E_UI_RENDER_QUICK");
        const std::string quickMode = quickRun ? quickRun : "";
        const bool quick = quickMode == "1" || quickMode == "narrow";
        // SF4E_UI_RENDER_QUICK=narrow is the same at 640x720, and SF4E_UI_RENDER_SHOTS=text writes only the shots whose name has it.
        const char* shotFilter = std::getenv("SF4E_UI_RENDER_SHOTS");
        const std::vector<Size> sizes = quickMode == "narrow" ? std::vector<Size>{{640,720,1.5f}} : quick ? std::vector<Size>{{1920,1080,1}} : translations ?
            std::vector<Size>{{640,720,1.5f}, {1280,720,1}, {1920,1080,1.5f}} :
            std::vector<Size>{{1280,720,1}, {1920,1080,1}, {1920,1080,1.25f}, {1920,1080,1.5f}, {2560,1440,1.5f}, {640,720,1.5f}, {3440,1440,1}, {3840,2160,1}, {3840,2160,1.5f}, {1280,720,2}};
        int frames = 0;
        // Overflows are collected rather than thrown, so one run lists every
        // row a translation needs shortened, with the locale and size it hit.
        std::string probeContext;
        std::set<std::string> overflows;
        sf4e::ui::SetMenuTextProbe([&](const char* id,float text,float interior,float width,float available){
            if(text>interior+.5f)overflows.insert(probeContext+" row "+id+": text height "+std::to_string(static_cast<int>(text))+" exceeds padded row "+std::to_string(static_cast<int>(interior)));
            if(width>available+.5f)overflows.insert(probeContext+" row "+id+": label width "+std::to_string(static_cast<int>(width))+" exceeds "+std::to_string(static_cast<int>(available)));
        });

        // A negative locale is the pseudo catalog.
        struct LocalePass { std::string name; int locale; };
        std::vector<LocalePass> localePasses;
        if(quick)localePasses={{"en",0}};
        else if(!translations)localePasses={{"en",0},{"pseudo",-1}};
        else for(int i=1;i<static_cast<int>(sf4e::loc::Locale::Count);++i)
            localePasses.push_back({sf4e::loc::Tag(static_cast<sf4e::loc::Locale>(i)),i});
        for(const auto& localePass:localePasses) {
          if(localePass.locale<0)sf4e::loc::testing::SetPseudoActive();
          else sf4e::loc::SetActive(static_cast<sf4e::loc::Locale>(localePass.locale));
        for(const auto size:sizes) {
            using namespace sf4e;using namespace ui;
            std::printf("UI viewport %dx%d at %.0f%% DPI\n",size.w,size.h,size.dpi*100);std::fflush(stdout);
            probeContext=localePass.name+" "+std::to_string(size.w)+"x"+std::to_string(size.h)+"@"+std::to_string(static_cast<int>(size.dpi*100))+"%";
            renderer.Resize(size.w,size.h);ImGui::CreateContext();
            auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(static_cast<float>(size.w),static_cast<float>(size.h));io.DeltaTime=1.f/60;
            ApplyTheme(size.dpi);ImGui_ImplDX9_Init(renderer.device);
            ApplicationShell shell;ShellView view;bool open=true;
            view.controllerReady=view.canChangeController=view.canEditPreferences=view.canEditSelection=view.canOpenRoom=view.helperReady=true;
            view.controller="Assigned controller";view.preferences.displayName="Ember Player";view.preferences.autoInputDelay=false;view.selectionSummary="Ryu / Original / Color 01 / Ultra I";
            FighterSelector selector;selection::Pick pick;int stage=0;selection::StageMask stagePool=(1u<<0)|(1u<<15);
            auto availability=[](int id){selection::Availability a;a.ready=true;a.personalActions=0x3ff;
                for(int c=0;c<selection::CostumeCount(id);++c){a.costumes|=1u<<c;a.colors[c]=(1u<<selection::ColorCount(id,c))-1;}return a;};
            training::View training;training.available=training.ready=training.checkpoint=true;training.generation=static_cast<unsigned>(size.w+size.dpi*100);
            training.lengths[0]=120;
            training::FrameMeter meter;std::array<training::FighterSample,2> fighters;
            for(int f=0;f<120;++f){
                for(int p=0;p<2;++p){auto& s=fighters[p];s.valid=true;s.timeScale=1;s.status=p?(f<40?0:f<65?22:0):(f<30?0:f<70?16:0);
                    s.action=s.status?100+p:0;s.actionFrame=static_cast<float>(f);s.firstActiveFrame=p?-1:34;s.lastActiveFrame=p?-1:38;s.health=p?920:1000;s.damage=p?80:0;s.comboDamage=p?160:0;
                    // A quarter circle and a heavy punch, pressed just before the attack starts.
                    s.input=p?0:f==24||f==25?2u:f==26||f==27?10u:f==28?8u:f>=29&&f<33?0x408u:0u;}
                meter.Observe(f,fighters);
            }
            training.meter=meter.View();training.history[0]={{0x14,5},{1,3},{0,16}};training.history[1]={{0x40,2},{0,10}};
            int mode=0;
            MatchStripView matchStrip;matchStrip.names[0]="Player One";matchStrip.names[1]="Player Two";
            matchStrip.links[0]=NetworkLink::Wired;matchStrip.links[1]=NetworkLink::Wireless;
            matchStrip.pingMs=68;matchStrip.rollbackFrames=2;matchStrip.appliedDelay=3;
            training::Command trainingCommand;
            bool acceptTraining=false,answerTickets=false,holdIdentity=false;
            std::uint64_t heldTicket=0;
            GameMenu recoveryMenu;recoveryMenu.navigation=RecoveryNavigation(false);
            platform::ServiceSnapshot recoveryState;bool recoveryUpdates=false;
            const auto draw=[&](const char* shot=nullptr,unsigned buttons=0,int settle=3){
                for(int i=0;i<settle;++i){
                    if(art)art->Pump();SetMenuInput({buttons,0});
                    if(mode==1){const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
                        for(unsigned key=0;key<6;++key)io.AddKeyEvent(keys[key],(buttons&(1u<<key))!=0);}
                    ImGui_ImplDX9_NewFrame();ImGui::NewFrame();
                    if(mode==0)shell.Draw(view,&open,[&](ShellAction a){
                        // An identity request is answered at once, successfully.
                        if(a.identity.op!=netplay::IdentityOp::None){
                            // Held: the helper has not answered, so a setup stays on its step until the shot answers by hand.
                            if(holdIdentity){heldTicket=a.identity.ticket;return true;}
                            view.identityTicket=a.identity.ticket;view.identityRequest=view.identity.requestId=view.identityTicket+1000;
                            view.identity.ok=true;view.identity.failure.clear();return true;
                        }
                        if(a.command.kind==netplay::CommandKind::SavePreferences&&view.settingsError.empty())view.preferences=a.preferences;
                        // A public room's ticket is answered with an admission to the room it asked for.
                        if(answerTickets&&a.tournament.op==netplay::tournament::Command::Op::RoomTicket){
                            view.publicRooms.answered=a.tournament.request;view.publicRooms.failure.clear();
                            view.publicRooms.admission.room.id=a.tournament.roomId;
                        }
                        return true;},[&]{selector.Draw(pick,true,art.get(),availability,&stage,view.canEditSelection,{},&stagePool);});
                    else if(mode==1)DrawTrainingFlyout(training,[&](training::Command c){trainingCommand=c;return acceptTraining;});
                    else if(mode==2)(void)DrawTrainingHud(training);
                    else if(mode==5)DrawControllerWarning("Match input blocked: reconnect your controller. If its slot changed, return to the room to reassign it.");
                    else if(mode==4)DrawRecoveryMenu(recoveryMenu,recoveryState,"The selected folder does not contain SSFIV.exe. Choose the installed game folder or close recovery without starting SF4.",recoveryUpdates);
                    else DrawMatchStrip(matchStrip);
                    CheckStacks();ImGui::Render();renderer.Draw();++frames;
                    if(mode==3)CheckMatchHudFrame(matchStrip,size.w,size.h);
                    if((mode==0||mode==4)&&i==settle-1&&settle>=3){
                        const auto* root=FindWindow(mode==0?"EmberShell":"###EmberRecovery");
                        if(root->ScrollMax.y>=1)throw std::runtime_error(std::string("Player menu footer escaped on ")+(mode==0?shell.Navigation().Screen():"recovery")+" by "+std::to_string(root->ScrollMax.y)+" pixels");
                    }
                    if(mode==1){
                        auto* flyout=FindWindow("###TrainingControls");
                        Require(flyout->Size.x<=size.w*.8f+1&&flyout->Size.y<=size.h*.8f+1,"Training flyout covers too much game");
                        Require(std::abs(flyout->Pos.x*2+flyout->Size.x-size.w)<=2&&flyout->Pos.y<=size.h*.05f,"Training flyout is not at the top centre");
                        Require(flyout->ScrollMax.y<1,"Training footer displaced by overflowing content");
                        Require(ImGui::GetTopMostPopupModal()==nullptr,"Training confirmation dims the game viewport");
                        for(auto* window:GImGui->Windows){
                            if(window->LastFrameActive!=ImGui::GetFrameCount()||window->RootWindow!=flyout)continue;
                            Require(window->Pos.x>=flyout->Pos.x-1&&window->Pos.y>=flyout->Pos.y-1&&
                                window->Pos.x+window->Size.x<=flyout->Pos.x+flyout->Size.x+1&&
                                window->Pos.y+window->Size.y<=flyout->Pos.y+flyout->Size.y+1,"Training child escapes the flyout");
                        }
                    }
                    const bool recoveryShot=shot && (std::string(shot).find("table-delay-")==0 ||
                        std::string(shot).find("room-transfer-host")==0 ||
                        std::string(shot).find("table-terminal-pending")==0 ||
                        std::string(shot).find("table-spectator-locked")==0 ||
                        std::string(shot).find("table-recover")==0 || std::string(shot).find("table-replacement")==0);
                    const bool matchShot=shot&&mode==3&&MatchHudShotCaptured(shot,size.w,size.h,size.dpi);
                    if(i==settle-1&&shot&&!output.empty()&&(!shotFilter||std::string(shot).find(shotFilter)!=std::string::npos)&&(!trainingShotsOnly||mode==1||mode==2)&&
                        (!recoveryShotsOnly||recoveryShot)&&(!matchShotsOnly||matchShot) && (!uxShotsOnly ||
                            ((std::string(shot)=="table-delay-checking" || std::string(shot)=="table-delay-retry" ||
                              std::string(shot)=="table-recover-updating" || std::string(shot)=="table-recover-leaving") &&
                             ((size.w==1280&&size.dpi==1) || (size.w==1920&&size.dpi==1.5f) || size.w==640))))
                        renderer.Capture(output+shot+"-"+localePass.name+"-"+std::to_string(size.w)+"-"+std::to_string(static_cast<int>(size.dpi*100))+".bmp",size.w,size.h);
                }
            };
            const auto page=[&](const char* screen){shell.Navigation().Home();if(std::string(screen)!="home")shell.Navigation().Push(screen);draw(screen);};
            SetMenuGlyphs(3,0x40000,0x20000);
            // The launch card opens once for mismatched game settings. Right
            // moves to "Don't show again" and Back declines it, so this covers
            // the two-button notice without writing the real preference.
            view.showGameSettingsCard=true;
            view.gameSettings.frameRate="SMOOTH";view.gameSettings.msaa="4X";
            draw("game-settings-card");
            Require(ImGui::GetTopMostPopupModal()!=nullptr,"Game settings card did not open");
            draw(nullptr,MenuInput::Right,1);draw();
            Require(ImGui::GetTopMostPopupModal()!=nullptr,"Choosing the alternative closed the card early");
            draw(nullptr,MenuInput::Back,1);draw();
            Require(ImGui::GetTopMostPopupModal()==nullptr,"Game settings card did not close");
            view.gameSettings={};view.showGameSettingsCard=false;
            for(const char* screen:{"home","profile","main-character","online","create","join","settings","player","defaults","interface","discord","about"})page(screen);
            page("replays");sf4e::replayinputs::Summary said;said.scored=true;said.score[0]=2;said.score[1]=1;said.rounds=3;said.frames=131*60;
            said.players[0]={1,2,9,-1,1,0};said.players[1]={2,0,0,-1,0,0};
            for(int side=0;side<2;side++){said.stats[side].frames=said.frames;said.stats[side].actions=700-side*40;said.stats[side].jumps=18;said.stats[side].crouched=said.frames*(54-side*23)/100;for(int b=0;b<6;b++)said.stats[side].presses[b]=101-b*14-side*9;}
            view.replays={{"a","2026-10-05 23:35",{"Alice","Bob"},false,true,false,said},{"b","2026-10-05 23:36",{},true,false}};view.replaysReady=true;view.replayNotice="Added as the newest entry of the game's replay list.";page("replays");
            view.replays.clear();view.replaysReady=false;view.replayNotice.clear();
            view.preferences.autoInputDelay=true;page("defaults");view.preferences.autoInputDelay=false;
            page("home");
            for(int i=0;i<8;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            draw("home-last-row");
            // Real editor activation and cancellation through the same input snapshot.
            page("player");draw(nullptr,MenuInput::Select,1);draw("text-edit");draw(nullptr,MenuInput::Back,1);draw();
            Require(!shell.Navigation().Editing()&&shell.Navigation().Screen()=="player","Text Back escaped the screen");
            // Long text opens in a reader; a language opens as a list; Home
            // keeps its help line under a status.
            page("about");draw(nullptr,MenuInput::Select,1);draw("about-reader");
            Require(shell.Navigation().Reading(),"About's controls did not open in the reader");
            draw(nullptr,MenuInput::Back,1);draw();Require(!shell.Navigation().Reading(),"Back did not close the reader");
            page("interface");
            for(int i=0;i<12&&shell.Navigation().Focus()!="language";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            draw(nullptr,MenuInput::Select,1);draw("language-choice");
            Require(shell.Navigation().Choosing(),"Select on Language did not open the list");
            draw(nullptr,MenuInput::Back,1);draw();
            view.error="The helper stopped responding. Ember is restarting it.";page("home");draw("home-status");view.error.clear();
            view.inputCapture=input::Capture::ReleaseAll;draw("controller-assignment");view.inputCapture=input::Capture::Idle;
            view.settingsError="The settings directory is temporarily unavailable.";page("settings");draw("save-error");
            page("main-character");
            for(int i=0;i<50&&shell.Navigation().Focus()!="retry-save";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            // Down preserves the grid column; the short final row may require
            // moving right to reach its last action.
            for(int i=0;i<8&&shell.Navigation().Focus()!="retry-save";++i){draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="retry-save","Portrait retry is unreachable");draw("portrait-save-error");
            view.settingsError.clear();shell=ApplicationShell{};
            page("selection");
            for(const char* screen:{"roster","appearance","costumes","colors","ultra","stage","random-pool","options"}){
                selector.Navigation().Home();selector.Navigation().Push(screen);
                if(argc>2&&!trainingShotsOnly&&(std::string(screen)=="costumes"||std::string(screen)=="colors")){
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                    bool loaded=false;
                    do{
                        art->Pump();loaded=true;
                        const auto a=availability(pick.fighter);
                        const auto options=std::string(screen)=="costumes"?selection::AllowedCostumes(pick.fighter,a):selection::AllowedColors(pick.fighter,pick.costume,a);
                        for(int option:options){
                            const auto img=std::string(screen)=="costumes"?art->Appearance(pick.fighter,option,0):art->Appearance(pick.fighter,pick.costume,option);
                            Require(!img.missing,"Gallery preview asset is missing");loaded=loaded&&img.texture!=0;
                        }
                        if(!loaded)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }while(!loaded&&std::chrono::steady_clock::now()<deadline);
                    Require(loaded,"Gallery preview loading timed out");
                }
                const bool gallery=std::string(screen)=="costumes"||std::string(screen)=="colors";
                if(gallery)SetMenuCardProbe([](const char* id,ImVec2 min,ImVec2 max){
                    if(!std::strcmp(id,"menu-back"))return;
                    Require(max.y-min.y<=ImGui::GetWindowHeight()+.5f,"Gallery card taller than the scrolling pane");
                    Require(max.y-min.y>=100,"Gallery artwork collapsed to an unreadable thumbnail");});
                draw(screen);SetMenuCardProbe({});
                if(std::string(screen)=="ultra"){
                    const int savedUltra=pick.ultra;
                    for(int ultra:{0,1,2}){
                        pick.ultra=ultra;draw(("ultra-saved-"+std::to_string(ultra)).c_str());
                    }
                    pick.ultra=savedUltra;
                }
            }
            // Select on the Random pool puts a skipped stage back (the page
            // opens on the Training Stage, which the fixture skips).
            selector.Navigation().Home();selector.Navigation().Push("random-pool");draw();
            draw(nullptr,MenuInput::Select,1);draw();
            Require(stagePool==(1u<<15),"Select did not put a skipped stage back in the Random pool");
            selector.Navigation().Home();selector.Navigation().Push("roster");draw();
            // The grid opens on its first card (not Ryu in USFIV's order), so two steps right land on a fighter other than the saved Ryu.
            const int saved=pick.fighter;draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Right,1);draw("fighter-focus");
            Require(pick.fighter==saved,"Grid movement committed fighter");
            draw(nullptr,MenuInput::Select,1);draw();Require(pick.fighter!=saved,"Select did not commit focused fighter");
            const int lockedFighter=pick.fighter;
            view.canEditSelection=false;draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Select,1);
            Require(pick.fighter==lockedFighter,"Locked fighter changed");draw("fighter-locked");view.canEditSelection=true;
            view.session.room=netplay::RoomState::Joined;view.session.control=netplay::Health::Healthy;view.session.generation.room=1;
            view.room.roomEpoch=42;view.room.name="Friday Night Fights";view.room.capacity=16;view.room.host=view.room.localMember=1;
            view.canEditPreferences=false;view.canReady=true;view.localSlot=0;view.invitation="sf4://test-only";
            for(int i=0;i<4;++i){auto& t=view.room.tables[i];t.id=i;t.revision=1;t.phase=i==0?room::TablePhase::Waiting:i==1?room::TablePhase::Playing:room::TablePhase::Idle;
                if(i<2){t.p1=i*2+1;t.p2=i*2+2;t.score[0]=i*120;t.score[1]=i*99;}}
            const char* sampleNames[]={"Ember Player","Akira","Jamie","Alex","Morgan","Riley","Sam","Jordan","Casey","Taylor","Robin","Ash","Sky","Reese","Avery","Drew"};
            for(int i=1;i<=16;++i){room::Member m;m.id=i;m.name=readmeShots?sampleNames[i-1]:(i==2?"Long player name for layout test":"Member "+std::to_string(i));m.host=i==1;m.fighter=(i-1)*2;m.mainFighter=(i+7)%44;
                if(i<=4){m.table=(i-1)/2;m.seat=(i-1)%2;m.status=i<3?room::MemberStatus::Seated:room::MemberStatus::Playing;}
                m.link=static_cast<NetworkLink>(i%3);
                // Idle readings on a seated member (hours) and on waiting members (minutes).
                if(!readmeShots&&(i==2||i>=5))m.idleSeconds=i==2?4500u:300u*i;
                view.room.members.push_back(m);}
            const char* sampleChat[]={"Welcome! Grab a table or join a queue.","Good games. I'll watch the next one.","Ready for another set?","Let's run it back!"};
            for(int i=0;i<20;++i)view.room.chat.push_back({static_cast<std::uint64_t>(i+1),static_cast<room::MemberId>(i%16+1),readmeShots?sampleChat[i%4]:"Ready for the next set? This is a longer chat message for narrow-layout inspection."});
            draw();for(const char* screen:{"room","room-table","room-members","room-chat","room-admin"})page(screen);
            ShootChat(shell,view,draw);
            // The seat chooser on an empty table, for a member with no seat.
            view.room.tables[0].p1=5;view.room.members[0].table=-1;view.room.members[0].seat=-1;
            page("room");
            for(int i=0;i<8&&shell.Navigation().Focus()!="table-2";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="table-2","An empty table is unreachable at this viewport/DPI");
            draw(nullptr,MenuInput::Select,1);draw("room-seat-chooser");
            Require(shell.Navigation().Choosing(),"A on an empty table did not open the seat chooser");
            draw(nullptr,MenuInput::Right,1);draw("room-seat-chooser-p2");
            draw(nullptr,MenuInput::Back,1);draw();
            // A full table offers the queue, watching and the table's options;
            // a watcher sees Stop watching instead.
            draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);
            Require(shell.Navigation().Focus()=="table-1","A full table is unreachable at this viewport/DPI");
            draw(nullptr,MenuInput::Select,1);draw("room-seat-chooser-full");
            Require(shell.Navigation().Choosing(),"A on a full table did not open the chooser");
            draw(nullptr,MenuInput::Back,1);draw();
            view.room.tables[1].spectators={1};
            draw(nullptr,MenuInput::Select,1);draw("room-seat-chooser-watching");
            draw(nullptr,MenuInput::Back,1);draw();view.room.tables[1].spectators.clear();
            for(int i=0;i<8&&shell.Navigation().Focus()!="table-0";++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
            view.room.tables[0].p1=1;view.room.members[0].table=0;view.room.members[0].seat=0;
            page("room-members");
            for(int i=0;i<24&&shell.Navigation().Focus()!="member-2";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="member-2","Host-transfer recipient is unreachable at this viewport/DPI");
            draw(nullptr,MenuInput::Select,1);draw();
            Require(shell.Navigation().Screen()=="room-member","Member activation did not open its actions");
            for(int i=0;i<12&&shell.Navigation().Focus()!="transfer-host";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="transfer-host","Host transfer is unreachable at this viewport/DPI");
            draw("room-transfer-host");
            draw(nullptr,MenuInput::Select,1);draw("room-transfer-host-confirm");
            Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),"Host transfer confirmation did not default to Cancel");
            draw(nullptr,MenuInput::Back,1);draw();
            page("room-table");view.room.tables[0].p2=0;view.canReady=false;draw("table-waiting-opponent");
            view.room.tables[0].p2=2;view.canReady=true;draw("table-ready-up");
            // Delay advice is a setup aid: the recommendation is read-only,
            // manual adjustment is bounded and immediate, and insufficient
            // probe data never blocks a manual Ready.
            view.recommendedDelay=4;view.selectedDelay=2;view.canProbe=true;view.canApplyDelay=true;
            view.probeStatus="complete";view.probeSamples=96;view.probeLost=4;draw("table-delay-recommended");
            view.probeStatus="checking";draw("table-delay-checking");
            view.probeStatus="unavailable";view.recommendedDelay=-1;draw("table-delay-retry");
            view.probeStatus="complete";view.recommendedDelay=4;
            for(const char* control:{"ultra","input-delay","check-connection"}) {
                for(int i=0;i<24&&shell.Navigation().Focus()!=control;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
                Require(shell.Navigation().Focus()==control,"Delay control is unreachable at this viewport/DPI");
                draw((std::string("table-delay-focus-")+control).c_str());
            }
            // Auto on the focused delay row: the delay it resolves to and its bounds.
            draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);
            Require(shell.Navigation().Focus()=="input-delay","Delay control is unreachable at this viewport/DPI");
            // Auto before its check, Auto once the opponent is measured, then one step Right: 1 frame.
            view.preferences.autoInputDelay=true;view.autoDelayMeasured=false;view.selectedDelay=2;draw("table-delay-auto-unmeasured");
            view.autoDelayMeasured=true;draw("table-delay-auto-measured");
            view.preferences.autoInputDelay=false;view.autoDelayMeasured=false;view.selectedDelay=1;draw("table-delay-auto-then-one");
            view.selectedDelay=2;
            draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);
            view.recommendedDelay=-1;view.canApplyDelay=false;view.probeStatus="insufficient samples";view.probeSamples=12;view.probeLost=88;
            view.canReady=true;draw("table-delay-manual-ready");
            view.room.tables[0].ready[0]=true;view.canReady=false;view.canEditSelection=false;draw("table-unready");
            view.room.tables[0].ready[1]=true;view.room.tables[0].phase=room::TablePhase::Playing;
            view.session.match=netplay::MatchState::PostMatch;draw("table-postmatch-waiting");
            view.room.tables[0].resultPending=true;draw("table-postmatch-reporting");
            view.room.tables[0].phase=room::TablePhase::Paused;draw("table-postmatch-unresolved");
            view.room.tables[0].phase=room::TablePhase::Waiting;
            view.room.tables[0].ready[0]=view.room.tables[0].ready[1]=false;
            view.room.tables[0].resultPending=false;view.canReady=view.canEditSelection=true;draw("table-rematch");
            // The opponent's new fighter: a line on the table card, with no modal.
            view.session.match=netplay::MatchState::None;view.opponentChangedFighter=10;++view.opponentChangeSequence;
            const int shownFighter=view.room.members[1].fighter;view.room.members[1].fighter=10;
            page("room");draw("room-opponent-changed");view.room.members[1].fighter=shownFighter;
            Require(!shell.NoticeOpen(),"The opponent's new fighter opened a modal notice");
            view.opponentChangedFighter=-1;page("room-table");view.session.match=netplay::MatchState::PostMatch;draw();
            // Applied terminal receipts remain a committed eligibility fence
            // until native/socket/helper retirement and explicit ACK.  Render
            // the waiting reason at every viewport/DPI so it cannot disappear
            // when the room table is narrow or scaled.
            view.room.localTerminalPending=true;view.room.terminalPending[0]=true;
            view.canReady=true;view.canEditSelection=true;
            // Ready itself stays pressable (the runtime parks it); the fighter
            // change control carries the committed waiting reason.
            // Fighter sits under Ready, above the rows focused so far.
            for(int i=0;i<24&&shell.Navigation().Focus()!="selection";++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
            for(int i=0;i<24&&shell.Navigation().Focus()!="selection";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="selection","Terminal waiting reason is unreachable at this viewport/DPI");
            draw("table-terminal-pending");
            view.room.localTerminalPending=false;view.room.terminalPending[0]=false;
            // A locked-in watcher: the lock-in row and its detail.
            view.room.tables[0].p1=5;view.room.tables[0].spectators={1};
            view.room.members[0].seat=-1;view.room.members[0].spectatorLocked=true;
            draw("table-spectator-locked");
            // The start held for this locked-in spectator: the card and the
            // lock-in row say the game waits for them.
            {
                auto& held=view.room.tables[0];
                held.phase=room::TablePhase::Ready;held.spectatorHold=true;held.ready[0]=held.ready[1]=true;held.holdRemainingMs=7000;
                view.room.localTerminalPending=true;
                page("room");draw("room-hold-spectator");page("room-table");
                for(int i=0;i<24&&shell.Navigation().Focus()!="lock-spectating";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
                Require(shell.Navigation().Focus()=="lock-spectating","The lock-in row is unreachable during a held start");
                draw("table-hold-spectator");
                // The watcher releases the lock-in before taking the seat
                // below, so no notice of why it ended covers the next shots.
                draw(nullptr,MenuInput::Select,1);draw(nullptr,0,1);
                view.room.localTerminalPending=false;
            }
            view.room.tables[0].p1=1;view.room.tables[0].spectators.clear();
            view.room.members[0].seat=0;view.room.members[0].spectatorLocked=false;
            // The same hold as the fighters see it: who they wait for, the time
            // left, and that B cancels the start.
            {
                auto& held=view.room.tables[0];
                held.spectators={6};view.room.members[5].spectatorLocked=true;view.canEditSelection=false;
                page("room");draw("room-hold-fighter");page("room-table");
                for(int i=0;i<24&&shell.Navigation().Focus()!="ready";++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
                Require(shell.Navigation().Focus()=="ready","Ready is unreachable during a held start");
                draw("table-hold-fighter");
                held.spectators.clear();view.room.members[5].spectatorLocked=false;view.canEditSelection=true;
                held.phase=room::TablePhase::Waiting;held.spectatorHold=false;held.ready[0]=held.ready[1]=false;held.holdRemainingMs=0;
            }
            // The watcher's rows have no fighter change, so focus moved; put it back.
            for(int i=0;i<24&&shell.Navigation().Focus()!="selection";++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
            for(int i=0;i<24&&shell.Navigation().Focus()!="selection";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="selection","Fighter change is unreachable after the watcher view");
            view.session.coordinated=true;view.session.authorityWritable=false;
            draw("table-recover-updating");
            view.session.authorityWritable=true;view.session.room=netplay::RoomState::Closing;
            draw("table-recover-leaving");
            view.session.room=netplay::RoomState::Joined;view.session.coordinated=false;
            view.session.recovery=netplay::Recovery::Recovering;
            view.session.error="Room control is recovering. Room actions are paused.";
            const auto recoveryFocus=shell.Navigation().Focus();
            std::string recoveryStatus;Tone recoveryTone=Tone::Neutral;
            SetMenuStatusProbe([&](const char* status,Tone tone){recoveryStatus=status;recoveryTone=tone;});
            draw();
            SetMenuStatusProbe({});
            Require(shell.Navigation().Focus()==recoveryFocus,"Recovery feedback displaced menu focus");
            Require(recoveryStatus==view.session.error,"Pinned recovery status lost its explanation");
            Require(recoveryTone==Tone::Error,"Room control failure is not rendered as an error");
            const auto* feedback=FindWindow("Command feedback");
            Require(feedback->Active&&feedback->DrawList->VtxBuffer.Size>0&&feedback->Size.y>0&&
                feedback->Pos.y>=0&&feedback->Pos.y+feedback->Size.y<=size.h,
                "Pinned recovery status is not visible at this viewport/DPI");
            draw("table-recovering");
            view.session.recovery=netplay::Recovery::ReplacementOffered;
            view.session.error="Room control unavailable. Replace the room when no match is active.";
            view.canReplaceRoom=true;
            // Replace room is on the board; the table page is only the table.
            page("room");draw();
            for(int i=0;i<64&&shell.Navigation().Focus()!="replace-room";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="replace-room","Replacement action is unreachable at this viewport/DPI");
            draw("table-replacement");
            view.room.tables[0].phase=room::TablePhase::Playing;view.session.match=netplay::MatchState::Preparing;
            view.canReady=false;view.canReplaceRoom=false;
            draw("table-replacement-waiting");
            view.canReplaceRoom=true;draw("table-replacement-preparation");
            view.session.match=netplay::MatchState::PostMatch;view.canReplaceRoom=true;
            draw("table-replacement-local-retired");
            draw(nullptr,MenuInput::Select,1);draw("table-replacement-local-retired-confirmation");
            Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),
                "Locally retired replacement confirmation is unsafe at this viewport/DPI");
            draw(nullptr,MenuInput::Back,1);draw();
            view.room.tables[0].phase=room::TablePhase::Waiting;view.session.match=netplay::MatchState::None;
            view.canReplaceRoom=false;view.session.recovery=netplay::Recovery::None;view.session.error.clear();
            view.session.match=netplay::MatchState::None;view.canReady=false;
            view.room.tables[0].ready[0]=false;view.canEditSelection=true;view.controllerReady=false;page("room-table");draw("table-controller-needed");
            view.controllerReady=view.canReady=true;
            const auto roomTitle=view.room.name;view.room.name=std::string(64,'W');page("room");draw("room-long-title");view.room.name=roomTitle;
            page("room");
            if(size.w-40*size.dpi>=820*size.dpi&&size.h/size.dpi>=700) {
                Require(FindWindow("Battle slots")->ScrollMax.y<2,"Four room battle slots must fit at standard landscape scale");
                // The wide board sizes its member list to whole cards, so the
                // bottom one is never cut through its portrait.
                const auto* members=FindWindow("Member list");
                const float pitch=58*Scale()+ImGui::GetStyle().ItemSpacing.y;
                const float content=members->Size.y-12*Scale()+ImGui::GetStyle().ItemSpacing.y;
                Require(content>=pitch-.5f&&std::fabs(content/pitch-std::floor(content/pitch+.5f))<.02f,
                    "Member list height is not a whole number of member cards");
            }
            // Move to Leave by clamped navigation, then open the safe dialog.
            for(int i=0;i<64&&shell.Navigation().Focus()!="leave";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            draw(nullptr,MenuInput::Select,1);draw("leave-confirmation");
            Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),"Leave confirmation not safe");
            auto* confirmation=FindWindow("###ConfirmAction");
            Require(confirmation&&confirmation->Size.x>(std::min)(300*size.dpi,size.w*.5f)&&confirmation->Size.x<size.w&&confirmation->Size.y<size.h,"Confirmation geometry unusable");
            draw(nullptr,MenuInput::Back,1);draw();
            view.discordPending=view.discordConfirm=view.discordCanSwitch=true;view.discordRevision=3;draw("discord-invitation");
            view.discordPending=false;draw();
            {
                // The Ember ID screens in each state that changes their rows.
                auto& id=view.identity;id=netplay::IdentityView{};id.known=true;
                id.state="disabled";id.passphraseRequired=true;page("identity");draw("identity-disabled-passphrase");
                id.state="locked";id.passphraseRequired=false;id.backend="passphrase";
                id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";
                page("identity");draw("identity-locked");
                id.state="recovery_required";page("identity");draw("identity-recovery");
                id.state="ready";id.backend="dpapi";page("identity");draw("identity-ready");
                id.exportPath="C:\\Users\\Player\\AppData\\Roaming\\sf4e\\identity-backups\\ember-id-20261001-120000.backup";
                page("identity-backup");draw("identity-backup");
                id.bridges={{"brg_00000000-0000-4000-8000-000000000001","https://tournaments.example","Example Tournaments"}};
                id.inspected=id.bridges[0];id.connections={{"blumint","BluMint"},{"mock-local","Mock provider"}};
                id.links={{"lnk_1","blumint","BluMint","PlayerOne"}};id.pending={{"clm_1","mock-local","Mock provider",{}}};
                // The page lists services, then inspects and lists the first.
                page("linked-accounts");draw(nullptr,0,12);draw("linked-accounts");
                Require(shell.Navigation().Screen()=="linked-accounts","Linked accounts did not open");
                // The matches page with a match in play, one to play and one
                // the organizer enters, then with none and no service.
                auto& t=view.tournament;t=netplay::tournament::Status{};
                t.list.bridge=id.bridges[0].id;
                netplay::tournament::Assignment match;match.matchId="emt_1";match.state="ready";match.profile="ember-room-v1";
                match.slot=0;match.gamesToWin=3;match.wins={1,2};match.roundLabel="Winners Round 1";match.opponentFingerprint="abcd1234-efgh5678";
                auto organizer=match;organizer.matchId="emt_2";organizer.profile="organizer-reported-v1";organizer.roundLabel="Grand Final";
                t.list.items={match,organizer};
                t.phase=netplay::tournament::Phase::InRoom;t.matchId="emt_1";t.waitingForPermit=true;
                page("tournament-matches");draw(nullptr,0,4);draw("tournament-matches");
                Require(shell.Navigation().Screen()=="tournament-matches","Tournament matches did not open");
                t=netplay::tournament::Status{};t.list.bridge=id.bridges[0].id;
                page("tournament-matches");draw(nullptr,0,4);draw("tournament-matches-none");
                id.state="disabled";page("linked-accounts");draw("linked-accounts-no-id");
                // Public rooms: the list, the card states, the setup cards and the room link (ui_render_public_rooms.hxx).
                ShootPublicRooms(shell,view,draw,page,[&]{if(ApplyTheme(size.dpi))ImGui_ImplDX9_InvalidateDeviceObjects();},answerTickets,holdIdentity,heldTicket);
                view.preferences.roomPublic=true;view.preferences.roomName="Open Mic";shell.Navigation().Home();draw(nullptr,0,4);
                page("create");draw(nullptr,0,8);draw("create-public");
                view.preferences.roomPublic=false;view.preferences.roomName="Private room";shell.Navigation().Home();draw(nullptr,0,4);
                view.identity=netplay::IdentityView{};view.tournament=netplay::tournament::Status{};view.publicRooms=netplay::publicrooms::Status{};
            }
            mode=1;TrainingNavigation().Home();draw("training-home");
            Require(TrainingNavigation().Focus()=="recording","Removed practice position still occupies training root");
            for(const char* screen:{"recording","history"}){
                TrainingNavigation().Home();TrainingNavigation().Push(screen);draw((std::string("training-")+screen).c_str());
            }
            // The combo creator's screens, with a combo on the Moves line to show.
            SetComboMoves("2MK > xx 236HP > FADC > 5HP > xx 623HP");
            for(const char* screen:{"combos","combo-timing","combo-moves"}){
                TrainingNavigation().Home();TrainingNavigation().Push("combos");if(std::string(screen)!="combos")TrainingNavigation().Push(screen);
                draw((std::string("training-")+screen).c_str());
            }
            SetComboMoves("");
            TrainingNavigation().Home();TrainingNavigation().Push("recording");draw();
            // Returning restores the prior selection, which may be below Record.
            for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
            for(int i=0;i<30&&TrainingNavigation().Focus()!="record";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(TrainingNavigation().Focus()=="record","Recording action unreachable");
            draw(nullptr,MenuInput::Select,1);draw("training-overwrite-confirmation");
            Require(TrainingNavigation().Confirming()&&!TrainingNavigation().ConfirmSelected(),"Training overwrite default is not Cancel");
            draw(nullptr,MenuInput::Back,1);draw();
            Require(TrainingNavigation().Screen()=="recording"&&!TrainingNavigation().Confirming(),"Training Back did more than cancel");
            acceptTraining=true;draw(nullptr,MenuInput::Down,1);draw();draw(nullptr,MenuInput::Select,1);draw("training-pending");
            training.commandId=trainingCommand.requestId;training.commandAccepted=false;
            training.commandError="Practice command rejected: the battle state changed while the command was pending. Wait until both fighters are ready and try again. This deliberately long explanation must not displace the controls or button legend.";
            draw("training-command-error");Require((TakeForwardedMenuAction().kind!=MenuAction::Close),"Failed training command closed flyout");
            training.ready=false;TrainingNavigation().Home();TrainingNavigation().Push("recording");draw("training-unavailable");
            training.ready=true;training.mode=training::Mode::Recording;draw("training-recording-suspended");training.mode=training::Mode::Idle;
            mode=2;draw("training-hud");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Passive training HUD captured input");
            CheckMatchHudScales();
            // Two bars, an input lane a fighter, the readings and the legend.
            auto* hud=FindWindow("Training frame meter");Require(hud->Size.x<=size.w*.76f&&hud->Size.y<size.h*.17f,"Passive HUD too large");
            Require(hud->Pos.y+hud->Size.y<=size.h*.83f,"Training HUD covers the game's super meters");
            SetMenuGlyphs(4,0,0);draw("training-hud-directinput");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"DirectInput HUD captured input");
            SetMenuGlyphs(0,0,0);draw("training-hud-keyboard");SetMenuGlyphs(3,0x40000,0x20000);
            // A running trial lists its steps beside the fight, as passive as the meter and clear of it.
            training.trialSteps.assign(14,"xx 236236PPP");training.trial.steps.assign(14,training::TrialStepState::Waiting);
            training.trial.steps[0]=training::TrialStepState::Done;training.trial.steps[1]=training::TrialStepState::Out;training.trial.steps[3]=training::TrialStepState::Unchecked;
            training.trial.current=1;training.trial.attempts=3;training.trial.successes=1;
            training.trial.lastFailure=training::TrialFailure::Whiffed;training.trial.failedStep=2;
            draw("training-hud-trial");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Trial list captured input");
            auto* list=FindWindow("Training trial");
            Require(list->Pos.x>=0&&list->Pos.x+list->Size.x<=size.w*.5f&&list->Pos.y+list->Size.y<=FindWindow("Training frame meter")->Pos.y,"Trial list leaves its corner");
            training.trialSteps.clear();training.trial=training::TrialView{};
            mode=3;draw("match-hud");
            ShootMatchHud(matchStrip,draw,[&]{ImGui_ImplDX9_InvalidateDeviceObjects();});
            mode=5;draw("controller-warning");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Controller warning captured input");
            auto* warning=FindWindow("Controller warning");
            Require(warning->ScrollMax.y<1&&warning->Pos.y+warning->Size.y<size.h,"Controller warning escaped viewport");
            mode=4;draw("launch-recovery");recoveryUpdates=true;recoveryState.installedVersion="1.1.0-rc1";recoveryState.channel=launcher::UpdateChannel::Beta;recoveryMenu.navigation=RecoveryNavigation(true);
            recoveryMenu.navigation.Prefer("channel");draw("update-channel-beta");
            recoveryState.channel=launcher::UpdateChannel::Nightly;recoveryState.installedVersion="1.2.0-nightly20261008";draw("update-channel-nightly");
            recoveryState.channel=launcher::UpdateChannel::Beta;recoveryState.installedVersion="1.1.0-rc1";recoveryMenu.navigation=RecoveryNavigation(true);recoveryState.update.ok=recoveryState.update.updateAvailable=true;
            recoveryState.update.expectedSha256=std::string(64,'a');recoveryState.update.latestVersion="v1.0.0";draw("update-available");
            // The found update is the first row, so Select asks to install it.
            draw(nullptr,MenuInput::Select,1);draw("update-confirmation");
            Require(recoveryMenu.navigation.Confirming()&&!recoveryMenu.navigation.ConfirmSelected(),"Recovery update confirmation is unsafe");
            draw(nullptr,MenuInput::Back,1);draw();recoveryState.pending=true;recoveryState.stageDone=25*1024*1024;recoveryState.stageTotal=100*1024*1024;
            recoveryState.message="Downloading the verified update. You can cancel this operation.";draw("update-downloading");
            mode=0;shell.Navigation().Home();draw("home-restored");
            auto* main=FindWindow("EmberShell");
            Require(main->Pos.x==0&&main->Pos.y==0&&main->Size.x==size.w&&main->Size.y==size.h,"Shell geometry changed");
            const float padding=ImGui::GetStyle().WindowPadding.x;
            ApplyTheme(size.dpi+.25f);ApplyTheme(size.dpi);ImGui_ImplDX9_InvalidateDeviceObjects();
            Require(ImGui::GetStyle().WindowPadding.x==padding,"DPI scaling accumulated");draw();
            ImGui_ImplDX9_InvalidateDeviceObjects();renderer.Resize(size.w,size.h);draw();
            ImGui_ImplDX9_Shutdown();ImGui::DestroyContext();
        }
        }
        sf4e::loc::SetActive(sf4e::loc::Locale::En);
        sf4e::ui::SetMenuTextProbe({});
        if(!overflows.empty()){
            for(const auto& overflow:overflows)std::fprintf(stderr,"Menu overflow: %s\n",overflow.c_str());
            std::fprintf(stderr,"UI render check failed: %d menu rows overflow\n",static_cast<int>(overflows.size()));
            return 1;
        }
        std::printf("Localized controller-first UI render checks passed: %d DX9 frames across %d locale/viewport/DPI configurations.\n",
            frames,static_cast<int>(localePasses.size()*sizes.size()));
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"UI render check failed: %s\n",error.what());return 1;}
}
