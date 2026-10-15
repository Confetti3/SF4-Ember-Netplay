#include "sf4e__CustomContent.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/StageCatalog.hxx"
#include "../platform/CustomContentFiles.hxx"

#include <array>
#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>
#include <intrin.h>
#include <detours/detours.h>
#include "spdlog/spdlog.h"

namespace sf4e { namespace custom {
namespace {
// A costume load request (both handlers below take it): type 1 = load, then the fighter, costume and color.
constexpr int RequestFighter = 0x64 / 4, RequestCostume = 0x68 / 4, RequestColor = 0x6c / 4;

// The match's swaps (UseAppearance), one per battle side.
std::array<selection::CostumeSwap, 2> costumeSwaps;
std::array<selection::ColorSwap, 2> colorSwaps;

using Handler = int (__thiscall*)(void*, int*);
Handler loadModels = nullptr, loadColors = nullptr;

// While the game builds a stand-in's file names (RYU_02.obj.emo ...), it is given the custom slot (RYU_12 ...).
int Load(Handler original, void* self, int* request) {
    if (request && request[0] == 1)
        for (const auto& s : costumeSwaps)
            if (s.custom >= 0 && request[RequestFighter] == s.fighter && request[RequestCostume] == s.standIn) {
                request[RequestCostume] = s.custom;
                const int result = original(self, request);
                request[RequestCostume] = s.standIn;
                return result;
            }
    return original(self, request);
}
int __fastcall LoadModels(void* self, void*, int* request) { return Load(loadModels, self, request); }
// The same for a stand-in color (RYU_01_01.col.emb ...): it is given the custom color (RYU_01_23 ...).
int __fastcall LoadColors(void* self, void*, int* request) {
    if (request && request[0] == 1)
        for (const auto& s : colorSwaps)
            if (s.custom >= 0 && request[RequestFighter] == s.fighter && request[RequestCostume] == s.costume &&
                request[RequestColor] == s.standIn) {
                request[RequestColor] = s.custom;
                const int result = Load(loadColors, self, request);   // a custom costume's color: its slot too
                request[RequestColor] = s.standIn;
                return result;
            }
    return Load(loadColors, self, request);
}

selection::InstalledContentPtr installed;
std::once_flag scanned;

// The stage loader's own calls of the stage code lookup (Steam 1.05: Stage::Loader::Load RVA 0x1f0f80 builds
// STG_<code>.emz/.tex.emz, the step after it RVA 0x1f0d10 runs <code>_SetupObj.lua, _SetupParam.lua, _CUBE.emb),
// and the stage music's (RVA 0x1efd20 builds battle/sound/bgm/BGM_<code>.csb; the call returns to RVA 0x1efdb2)
// when the custom stage has music of its own. Everything else keeps the fallback stage's code.
using StageCode = const char* (__cdecl*)(int);
StageCode stageCode = nullptr;
std::uintptr_t loaderBegin = 0, loaderEnd = 0, musicCaller = 0;
int customNative = -1;
char customCode[4] = {};    // the custom stage's code while one is loaded, else empty
bool customMusic = false;
std::wstring battleFolder;   // patch_ae2_tu3\battle\ beside the game

const char* __cdecl LoaderStageCode(int stageId) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (customCode[0] && stageId == customNative &&
        ((caller >= loaderBegin && caller < loaderEnd) || (customMusic && caller == musicCaller))) return customCode;
    return stageCode(stageId);
}

// The optional stage music must also be a regular file.
bool Exists(const std::wstring& file) {
    const DWORD attributes = GetFileAttributesW(file.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

}

selection::InstalledContentPtr Installed() {
    std::call_once(scanned, [] {
        wchar_t exe[32768] = {};
        const DWORD length = GetModuleFileNameW(NULL, exe, 32768);
        if (!length || length >= 32768) {
            installed = std::make_shared<const selection::InstalledContent>();
            return;
        }
        const std::wstring path(exe, length);
        battleFolder = path.substr(0, path.find_last_of(L"\\/")) + L"\\patch_ae2_tu3\\battle\\";
        installed = EnvFlag("SF4E_IGNORE_CUSTOM") ? std::make_shared<const selection::InstalledContent>() :
            platform::custom_content::Scan(battleFolder);
        spdlog::info("Custom content scanned: {} stages{}", installed->stages.size(),
            EnvFlag("SF4E_IGNORE_CUSTOM") ? " (ignored for this copy)" : "");
    });
    return installed;
}

int ApplyStage(int stageId) {
    customNative = -1;
    customCode[0] = 0;
    customMusic = false;
    if (!selection::IsCustomStage(stageId)) return stageId;
    const int fallback = selection::CustomStageFallback(stageId);
    if (!Installed()->HasStage(stageId)) return fallback;
    const std::string code = selection::StageCode(stageId);
    std::memcpy(customCode, code.c_str(), sizeof(customCode));
    customNative = fallback;
    customMusic = Exists(battleFolder + L"sound\\bgm\\BGM_" + std::wstring(code.begin(), code.end()) + L".csb");
    spdlog::info("Custom stage {} plays as stage {} with its own files{}", code, fallback, customMusic ? " and music" : "");
    return fallback;
}

void UseAppearance(const selection::Appearance& appearance) {
    for (int side = 0; side < 2; side++) {
        costumeSwaps[side] = appearance.costumes[side];
        colorSwaps[side] = appearance.colors[side];
        const auto& c = costumeSwaps[side];
        if (c.custom >= 0) spdlog::info("Custom costume: side {} fighter {} slot {} plays as costume {}", side, c.fighter, c.custom + 1, c.standIn + 1);
        const auto& k = colorSwaps[side];
        if (k.custom >= 0)
            spdlog::info("Custom color: side {} fighter {} costume {} color {} plays as color {}", side, k.fighter, k.costume + 1, k.custom + 1, k.standIn + 1);
    }
}

void EndBattle() {
    costumeSwaps = {};
    colorSwaps = {};
    customNative = -1;
    customCode[0] = 0;
    customMusic = false;
}

void Install() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL));
    loadModels = reinterpret_cast<Handler>(base + 0x1e5f80);
    loadColors = reinterpret_cast<Handler>(base + 0x1e5cf0);
    DetourAttach((PVOID*)&loadModels, (PVOID)LoadModels);
    DetourAttach((PVOID*)&loadColors, (PVOID)LoadColors);
    stageCode = reinterpret_cast<StageCode>(base + 0x2862c0);
    loaderBegin = base + 0x1f0d10;
    loaderEnd = base + 0x1f1100;
    musicCaller = base + 0x1efdb2;
    DetourAttach((PVOID*)&stageCode, (PVOID)LoaderStageCode);
}

} }
