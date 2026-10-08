#include "sf4e__CustomContent.hxx"
#include "../Dimps/Dimps__Selection.hxx"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>

#include <windows.h>
#include <intrin.h>
#include <detours/detours.h>
#include "spdlog/spdlog.h"

namespace sf4e { namespace custom {
namespace {
// A costume load request (both handlers below take it): type 1 = load, then the fighter, costume and colour.
constexpr int RequestFighter = 0x64 / 4, RequestCostume = 0x68 / 4;

struct StandIn { int fighter = -1, costume = -1, custom = -1; };
std::array<StandIn, 2> standIns;

using Handler = int (__thiscall*)(void*, int*);
Handler loadModels = nullptr, loadColours = nullptr;

// While the game builds a stand-in's file names (RYU_02.obj.emo ...), it is given the custom slot (RYU_12 ...).
int Load(Handler original, void* self, int* request) {
    if (request && request[0] == 1)
        for (const auto& s : standIns)
            if (s.custom >= 0 && request[RequestFighter] == s.fighter && request[RequestCostume] == s.costume) {
                request[RequestCostume] = s.custom;
                const int result = original(self, request);
                request[RequestCostume] = s.costume;
                return result;
            }
    return original(self, request);
}
int __fastcall LoadModels(void* self, void*, int* request) { return Load(loadModels, self, request); }
int __fastcall LoadColours(void* self, void*, int* request) { return Load(loadColours, self, request); }

std::array<std::vector<int>, selection::FighterCount> installed;
std::vector<int> installedStages;
std::once_flag scanned;

// The stage loader's own calls of the stage code lookup (Steam 1.05: Stage::Loader::Load RVA 0x1f0f80 builds
// STG_<code>.emz/.tex.emz, the step after it RVA 0x1f0d10 runs <code>_SetupObj.lua, _SetupParam.lua, _CUBE.emb),
// and the stage music's (RVA 0x1efd20 builds battle/sound/bgm/BGM_<code>.csb; the call returns to RVA 0x1efdb2)
// when the custom stage has music of its own. Everything else keeps the fallback stage's code.
using StageCode = const char* (__cdecl*)(int);
StageCode stageCode = nullptr;
std::uintptr_t loaderBegin = 0, loaderEnd = 0, musicCaller = 0;
int customNative = -1;
const char* customCode = nullptr;
bool customMusic = false;
std::wstring battleFolder;   // patch_ae2_tu3\battle\ beside the game

const char* __cdecl LoaderStageCode(int stageId) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (customCode && stageId == customNative &&
        ((caller >= loaderBegin && caller < loaderEnd) || (customMusic && caller == musicCaller))) return customCode;
    return stageCode(stageId);
}

void Scan() {
    wchar_t ignore[8] = {};
    if (GetEnvironmentVariableW(L"SF4E_IGNORE_CUSTOM", ignore, 8) && ignore[0] && ignore[0] != L'0') {
        spdlog::info("Custom costumes: ignored for this copy (SF4E_IGNORE_CUSTOM)");
        return;
    }
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    std::wstring battle(exe);
    battle = battle.substr(0, battle.find_last_of(L'\\')) + L"\\patch_ae2_tu3\\battle\\";
    battleFolder = battle;
    const std::wstring root = battle + L"chara\\";
    int total = 0;
    for (int id = 0; id < selection::FighterCount; id++) {
        const std::string code = selection::FindFighter(id)->code;
        const std::wstring chr(code.begin(), code.end());
        for (int costume = selection::FirstCustomCostume; costume < selection::CostumeLimit; costume++) {
            const int slot = costume + 1;
            const std::wstring file = root + chr + L"\\" + chr + L"_" + std::to_wstring(slot / 10) +
                                      std::to_wstring(slot % 10) + L".obj.emo";
            if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) installed[id].push_back(costume);
        }
        total += static_cast<int>(installed[id].size());
    }
    spdlog::info("Custom costumes installed: {}", total);
    // STG_<code>.emz with a custom code (the pattern also finds STG_<code>.tex.emz and the game's own stages).
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileW((battle + L"stage\\STG_*.emz").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name.size() != 11 || name[4] > 127 || name[5] > 127 || name[6] > 127) continue;
            const char code[4] = {static_cast<char>(name[4]), static_cast<char>(name[5]), static_cast<char>(name[6]), 0};
            const int id = selection::CustomStageId(code);
            if (id >= 0) installedStages.push_back(id);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    std::sort(installedStages.begin(), installedStages.end());
    spdlog::info("Custom stages installed: {}", installedStages.size());
}
}

const std::vector<int>& InstalledCostumes(int fighterId) {
    static const std::vector<int> none;
    if (!selection::FindFighter(fighterId)) return none;
    std::call_once(scanned, Scan);
    return installed[fighterId];
}

const std::vector<int>& InstalledStages() {
    std::call_once(scanned, Scan);
    return installedStages;
}

selection::Availability ReadAvailability(int fighterId) {
    selection::Availability result = Dimps::Selection::ReadAvailability(fighterId);
    result.customCostumes = InstalledCostumes(fighterId);
    result.customStages = InstalledStages();
    return result;
}

int ApplyStage(int stageId) {
    customNative = -1;
    customCode = nullptr;
    customMusic = false;
    if (!selection::IsCustomStage(stageId)) return stageId;
    const int fallback = selection::CustomStageFallback(stageId);
    for (int id : InstalledStages())
        if (id == stageId) {
            customNative = fallback;
            customCode = selection::FindStage(stageId)->code;
            const std::string code = customCode;
            customMusic = GetFileAttributesW((battleFolder + L"sound\\bgm\\BGM_" + std::wstring(code.begin(), code.end()) + L".csb").c_str()) !=
                          INVALID_FILE_ATTRIBUTES;
            spdlog::info("Custom stage {} plays as stage {} with its own files{}", customCode, fallback, customMusic ? " and music" : "");
        }
    return fallback;
}

bool CostumeInstalled(int fighterId, int costumeId) {
    for (int costume : InstalledCostumes(fighterId))
        if (costume == costumeId) return true;
    return false;
}

void SetStandIn(int side, int fighter, int standIn, int custom) {
    if (side < 0 || side > 1) return;
    standIns[side] = { fighter, standIn, custom };
    if (custom >= 0) spdlog::info("Custom costume: side {} fighter {} slot {} plays as costume {}", side, fighter, custom + 1, standIn + 1);
}

// Main menu music: a random stage theme from the game's own lobby pool (stages 1-21, as
// Dimps::GameEvents::Network::LobbySoundPlayer picks them) instead of looping BGM_MAIN, picked at start-up and after
// every match; the menu reads its music's path from a table entry (RVA 0x66b6f0) each time it loads it.
// Remove this function and its two calls to get the menu's own track back.
void PickMenuMusic() {
    static std::string path;
    path = std::string("battle/sound/bgm/BGM_") + selection::FindStage(1 + GetTickCount() % 21)->code + ".csb";
    *reinterpret_cast<const char**>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL)) + 0x66b6f0) = path.c_str();
}

void EndBattle() {
    standIns = {};
    customNative = -1;
    customCode = nullptr;
    customMusic = false;
    PickMenuMusic();
}

void Install() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL));
    PickMenuMusic();
    loadModels = reinterpret_cast<Handler>(base + 0x1e5f80);
    loadColours = reinterpret_cast<Handler>(base + 0x1e5cf0);
    DetourAttach((PVOID*)&loadModels, (PVOID)LoadModels);
    DetourAttach((PVOID*)&loadColours, (PVOID)LoadColours);
    stageCode = reinterpret_cast<StageCode>(base + 0x2862c0);
    loaderBegin = base + 0x1f0d10;
    loaderEnd = base + 0x1f1100;
    musicCaller = base + 0x1efdb2;
    DetourAttach((PVOID*)&stageCode, (PVOID)LoaderStageCode);
}

} }
