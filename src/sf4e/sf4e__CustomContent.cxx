#include "sf4e__CustomContent.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/StageCatalog.hxx"
#include "../Dimps/Dimps__Selection.hxx"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
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

struct StandIn { int fighter = -1, costume = -1, custom = -1; };
std::array<StandIn, 2> standIns;
struct ColorStandIn { int fighter = -1, costume = -1, color = -1, custom = -1; };
std::array<ColorStandIn, 2> colorStandIns;

using Handler = int (__thiscall*)(void*, int*);
Handler loadModels = nullptr, loadColors = nullptr;

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
// The same for a stand-in color (RYU_01_01.col.emb ...): it is given the custom color (RYU_01_23 ...).
int __fastcall LoadColors(void* self, void*, int* request) {
    if (request && request[0] == 1)
        for (const auto& s : colorStandIns)
            if (s.custom >= 0 && request[RequestFighter] == s.fighter && request[RequestCostume] == s.costume &&
                request[RequestColor] == s.color) {
                request[RequestColor] = s.custom;
                const int result = loadColors(self, request);
                request[RequestColor] = s.color;
                return result;
            }
    return Load(loadColors, self, request);
}

std::array<std::vector<int>, selection::FighterCount> installed;
std::array<std::array<std::vector<int>, 8>, selection::FighterCount> installedColors;
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

// The name the setup program gives an item: the first line of the .txt beside it (UTF-8), at most 40 bytes.
std::string ReadName(const std::wstring& file) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c >= 0 && c < ' '; }), line.end());
    if (line.size() > 40) {
        std::size_t cut = 40;
        while (cut > 0 && (line[cut] & 0xC0) == 0x80) cut--;
        line.resize(cut);
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    return line;
}

bool Exists(const std::wstring& file) { return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES; }
// The two digits at `at` of a file name (RYU_12...), or -1.
int Number(const std::wstring& name, std::size_t at) {
    if (name.size() < at + 2 || name[at] < L'0' || name[at] > L'9' || name[at + 1] < L'0' || name[at + 1] > L'9') return -1;
    return (name[at] - L'0') * 10 + (name[at + 1] - L'0');
}
// Calls `found` with each file name matching `pattern`.
template<class F> void Each(const std::wstring& pattern, F found) {
    WIN32_FIND_DATAW data;
    const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do found(std::wstring(data.cFileName)); while (FindNextFileW(search, &data));
    FindClose(search);
}

void Scan() {
    if (EnvFlag("SF4E_IGNORE_CUSTOM")) {
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
        const std::wstring chr(code.begin(), code.end()), folder = root + chr + L"\\";
        // A color is <name>.col.emb and <name>.obj.emm.
        const auto colorFiles = [&](const std::wstring& name) { return Exists(folder + name + L".col.emb") && Exists(folder + name + L".obj.emm"); };
        // Custom costumes: <CHR>_<NN>.obj.emo, NN 08..99, with all ten colors (an opponent may pick any of them).
        Each(folder + chr + L"_??.obj.emo", [&](const std::wstring& name) {
            const int costume = Number(name, 4) - 1;
            if (name.size() != 14 || !selection::IsCustomCostume(costume)) return;
            for (int color = 1; color <= selection::CustomColorCount; color++)
                if (!colorFiles(name.substr(0, 6) + L"_" + std::to_wstring(color / 10) + std::to_wstring(color % 10))) return;
            installed[id].push_back(costume);
            selection::SetCustomName(id, costume, -1, ReadName(folder + name.substr(0, 6) + L".txt"));
        });
        std::sort(installed[id].begin(), installed[id].end());
        total += static_cast<int>(installed[id].size());
        // Custom colors of the game's costumes: <CHR>_<CC>_<NN>, NN 30..99.
        Each(folder + chr + L"_??_??.col.emb", [&](const std::wstring& name) {
            const int costume = Number(name, 4) - 1, colorId = Number(name, 7) - 1;
            if (name.size() != 17 || costume < 0 || costume >= selection::CostumeCount(id) || !selection::IsCustomColor(colorId) ||
                !colorFiles(name.substr(0, 9))) return;
            installedColors[id][costume].push_back(colorId);
            selection::SetCustomName(id, costume, colorId, ReadName(folder + name.substr(0, 9) + L".txt"));
        });
        for (auto& list : installedColors[id]) std::sort(list.begin(), list.end());
    }
    spdlog::info("Custom costumes installed: {}", total);
    // Custom stages: STG_<code>.emz and STG_<code>.tex.emz with a custom code (the pattern also finds the game's own).
    Each(battle + L"stage\\STG_???.emz", [&](const std::wstring& name) {
        if (name.size() != 11 || name[4] > 127 || name[5] > 127 || name[6] > 127) return;
        const char code[4] = {static_cast<char>(name[4]), static_cast<char>(name[5]), static_cast<char>(name[6]), 0};
        const int id = selection::CustomStageId(code);
        if (id < 0 || !Exists(battle + L"stage\\" + name.substr(0, 7) + L".tex.emz")) return;
        installedStages.push_back(id);
        selection::SetCustomStageName(id, ReadName(battle + L"stage\\" + name.substr(0, 7) + L".txt").c_str());
    });
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

const std::vector<int>& InstalledColors(int fighterId, int costumeId) {
    static const std::vector<int> none;
    if (!selection::FindFighter(fighterId) || costumeId < 0 || costumeId >= 8) return none;
    std::call_once(scanned, Scan);
    return installedColors[fighterId][costumeId];
}

bool ColorInstalled(int fighterId, int costumeId, int colorId) {
    const auto& colors = InstalledColors(fighterId, costumeId);
    return std::find(colors.begin(), colors.end(), colorId) != colors.end();
}

selection::Availability ReadAvailability(int fighterId) {
    selection::Availability result = Dimps::Selection::ReadAvailability(fighterId);
    result.customCostumes = InstalledCostumes(fighterId);
    result.customStages = InstalledStages();
    for (int costume = 0; costume < static_cast<int>(result.customColors.size()); costume++)
        result.customColors[costume] = InstalledColors(fighterId, costume);
    return result;
}

int ApplyStage(int stageId) {
    customNative = -1;
    customCode = nullptr;
    customMusic = false;
    if (!selection::IsCustomStage(stageId)) return stageId;
    const int fallback = selection::CustomStageFallback(stageId);
    const auto& stages = InstalledStages();
    if (!std::binary_search(stages.begin(), stages.end(), stageId)) return fallback;
    customNative = fallback;
    customCode = selection::FindStage(stageId)->code;
    const std::string code = customCode;
    customMusic = Exists(battleFolder + L"sound\\bgm\\BGM_" + std::wstring(code.begin(), code.end()) + L".csb");
    spdlog::info("Custom stage {} plays as stage {} with its own files{}", customCode, fallback, customMusic ? " and music" : "");
    return fallback;
}

bool CostumeInstalled(int fighterId, int costumeId) {
    const auto& costumes = InstalledCostumes(fighterId);
    return std::find(costumes.begin(), costumes.end(), costumeId) != costumes.end();
}

void SetStandIn(int side, int fighter, int standIn, int custom) {
    if (side < 0 || side > 1) return;
    standIns[side] = { fighter, standIn, custom };
    if (custom >= 0) spdlog::info("Custom costume: side {} fighter {} slot {} plays as costume {}", side, fighter, custom + 1, standIn + 1);
}

void SetColorStandIn(int side, int fighter, int costume, int standIn, int custom) {
    if (side < 0 || side > 1) return;
    colorStandIns[side] = { fighter, costume, standIn, custom };
    if (custom >= 0) spdlog::info("Custom color: side {} fighter {} costume {} color {} plays as color {}", side, fighter, costume + 1, custom + 1, standIn + 1);
}

// Main menu music: a random stage theme from the game's own lobby pool (stages 1-21, as
// Dimps::GameEvents::Network::LobbySoundPlayer picks them) instead of looping BGM_MAIN, picked at start-up and after
// every match; the menu reads its music's path from a table entry (RVA 0x66b6f0) each time it loads it.
// Remove this function and its two calls to get the menu's own track back.
void PickMenuMusic() {
    static char path[] = "battle/sound/bgm/BGM_CHN.csb";   // one lasting buffer: only the stage code changes
    std::memcpy(path + 21, selection::FindStage(1 + GetTickCount() % 21)->code, 3);
    *reinterpret_cast<const char**>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL)) + 0x66b6f0) = path;
}

void EndBattle() {
    standIns = {};
    colorStandIns = {};
    customNative = -1;
    customCode = nullptr;
    customMusic = false;
    PickMenuMusic();
}

void Install() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL));
    PickMenuMusic();
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
