#include "sf4e__Music.hxx"
#include "../common/MusicPlaybackState.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/Localization.hxx"
#include <windows.h>
#include <intrin.h>
#include <detours/detours.h>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <random>
#include <spdlog/spdlog.h>

namespace sf4e { namespace Music {
namespace {
// Static disassembly of the owned SSFIV.exe (Steam 1.05), SHA-256
// 5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb.
// Sound sheet create: (path, flags, streaming, arg4, arg5), cdecl.
// UI background BGM owns two sheets at +0x18/+0x1c, and two player handles
// at +0/+4. Its own Load/Ready/Play/Release retain the game's volume, looping,
// async loading and native sound pool ownership. No external decoder/player.
using CreateSheet = void* (__cdecl*)(const char*, unsigned, int, int, int);
using MenuMethod = void (__thiscall*)(void*);
using MenuReady = int (__thiscall*)(void*);
using MenuPlay = void (__thiscall*)(void*, int);
struct BattleBgmInit { unsigned sheet; int mode; };
static_assert(sizeof(BattleBgmInit) == 8, "Native BgmPlayer init has two 32-bit fields");
using BattleBgmConstructor = void* (__thiscall*)(void*, const BattleBgmInit*);
CreateSheet createSheet = nullptr;
BattleBgmConstructor battleBgmConstructor = nullptr;
MenuMethod menuUpdate = nullptr, menuRelease = nullptr, menuLoad = nullptr;
MenuReady menuReady = nullptr;
MenuPlay menuPlay = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> available{false}, matchArmed{false};
std::atomic<int> matchChoice{music::Default};
bool attached = false, initialized = false, safe = false, room = false;
thread_local bool reloading = false;
int lobbyChoice = music::Default;
int configuredMatch = music::Default;
thread_local int loadingChoice = music::Default;
std::array<std::string, music::CharacterBase + selection::FighterCount> paths;
std::array<bool, music::CharacterBase + selection::FighterCount> installed{};
std::vector<int> installedTracks;
std::mt19937 randomMusic;
enum class ErrorCode { None, fallback, retry_default, load_failed, disabled, unsupported, hook_failed, files_unavailable, missing };
ErrorCode error=ErrorCode::None;
music::LobbyPlaybackState playback;
void* lastManager = nullptr;
ULONGLONG loadStarted = 0;
bool fallingBack = false, loadFailed = false;

bool Exists(const std::wstring& path) {
    const auto attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
int InstalledChoice(int choice) {
    return music::Find(choice) && installed[choice] ? choice : music::Default;
}
const char* Replacement(int choice, const char* original) {
    return choice == music::Default ? original : paths[choice].c_str();
}
void* __cdecl Create(const char* path, unsigned flags, int streaming, int arg4, int arg5) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - base;
    // Only the primary UI background bank while this module reloads it.
    // No menu effects, character sounds, announcer or native fight state changes.
    if (available && path && reloading && caller == 0x2869a7 &&
        std::strcmp(path, "ui/sound/bgm/BGM_MAIN.csb") == 0)
        path = Replacement(loadingChoice, path);
    // The battle's BGM loader, not its SE/voice resource constructors.
    else if (available && path && matchArmed && caller == 0x1efedd &&
        std::strncmp(path, "battle/sound/bgm/BGM_", 21) == 0)
        path = Replacement(InstalledChoice(matchChoice), path);
    return createSheet(path, flags, streaming, arg4, arg5);
}
void* __fastcall ConstructBattleBgm(void* self, void*, const BattleBgmInit* init) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - base;
    const int choice = InstalledChoice(matchChoice);
    if (available && matchArmed && init && choice != music::Default && caller == 0x1928e8) {
        // The constructor copies both fields; it does not retain this pointer.
        // Let its native single-cue path ignore unsupported phase changes,
        // instead of crossfading Sakura's cue 0 into nonexistent cues 1/2.
        auto adjusted = *init;
        adjusted.mode = music::BattleCueMode(choice, init->mode);
        spdlog::info("Music: battle player track={} native_mode={} selected_mode={}", choice, init->mode, adjusted.mode);
        return battleBgmConstructor(self, &adjusted);
    }
    return battleBgmConstructor(self, init);
}
void Reload(void* manager, int choice) {
    reloading = true;
    loadingChoice = choice;
    menuRelease(manager);
    menuLoad(manager);
    reloading = false;
    loadStarted = GetTickCount64();
}
void __fastcall Release(void* self, void*) {
    menuRelease(self);
    if (!reloading && self == lastManager) playback.Reset();
}
void __fastcall Update(void* self, void*) {
    menuUpdate(self);
    if (self != lastManager) { lastManager = self; playback.Reset(); }
    if (!available || !initialized || !safe) return;
    const int wanted = room ? InstalledChoice(lobbyChoice) : music::Default;
    if (playback.Request(wanted, safe)) {
        fallingBack = false;
        spdlog::info("Music: loading lobby track {}", wanted);
        Reload(self, wanted);
    }
    if (!playback.pending) return;
    if (menuReady(self)) {
        menuPlay(self, 0);
        playback.pending = false;
        spdlog::info("Music: lobby bank ready (track={}, fallback={})", playback.applied, fallingBack);
        if (fallingBack) error = ErrorCode::fallback;
    } else if (GetTickCount64() - loadStarted >= 10000) {
        loadFailed = true;
        // Fall back once, without retrying a bad bank every frame. The original
        // game owns cleanup on its next scene change. Retry on a new selection.
        if (!fallingBack) {
            error = ErrorCode::retry_default;
            fallingBack = true; Reload(self, music::Default);
        } else {
            error = ErrorCode::load_failed;
            playback.pending = false;
        }
    }
}
template<std::size_t N> bool Bytes(std::uintptr_t rva, const unsigned char (&expected)[N]) {
    return std::memcmp(reinterpret_cast<const void*>(base + rva), expected, N) == 0;
}
}

void Install() {
    if (EnvFlag("SF4E_DISABLE_MUSIC")) { error = ErrorCode::disabled; return; }
    base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Reject a different engine layout before attaching any of these hooks.
    const unsigned char create[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x3b,0xea,0x8f,0x00};
    const unsigned char update[] = {0x56,0x8b,0xf1,0x8d,0x4e,0x18,0xe8};
    const unsigned char release[] = {0x56,0x57,0x8b,0xf9,0xe8};
    const unsigned char load[] = {0x53,0x8b,0xd9,0x57,0x8d,0x4b,0x18};
    const unsigned char play[] = {0x55,0x8b,0xec,0x53,0x8b,0x5d,0x08,0x56};
    const unsigned char ready[] = {0x83,0xc1,0x18,0xe8};
    const unsigned char battleConstructor[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x38,0x29,0x90,0x00};
    if (!Bytes(0x275e40, create) || !Bytes(0x286940, update) || !Bytes(0x286620, release) ||
        !Bytes(0x2865b0, load) || !Bytes(0x286680, play) || !Bytes(0x286460, ready) ||
        !Bytes(0x192630, battleConstructor)) {
        error = ErrorCode::unsupported; return;
    }
    createSheet = reinterpret_cast<CreateSheet>(base + 0x275e40);
    menuUpdate = reinterpret_cast<MenuMethod>(base + 0x286940);
    menuRelease = reinterpret_cast<MenuMethod>(base + 0x286620);
    menuLoad = reinterpret_cast<MenuMethod>(base + 0x2865b0);
    menuPlay = reinterpret_cast<MenuPlay>(base + 0x286680);
    menuReady = reinterpret_cast<MenuReady>(base + 0x286460);
    battleBgmConstructor = reinterpret_cast<BattleBgmConstructor>(base + 0x192630);
    // Sidecar owns transaction failure/rollback and does not launch on failure.
    const auto a = DetourAttach(reinterpret_cast<PVOID*>(&createSheet), reinterpret_cast<PVOID>(Create));
    const auto b = DetourAttach(reinterpret_cast<PVOID*>(&menuUpdate), reinterpret_cast<PVOID>(Update));
    const auto c = DetourAttach(reinterpret_cast<PVOID*>(&menuRelease), reinterpret_cast<PVOID>(Release));
    const auto d = DetourAttach(reinterpret_cast<PVOID*>(&battleBgmConstructor), reinterpret_cast<PVOID>(ConstructBattleBgm));
    attached = a == NO_ERROR && b == NO_ERROR && c == NO_ERROR && d == NO_ERROR;
    if (!attached) error = ErrorCode::hook_failed;
}
void Activate() { available = attached; }
bool Available() { return available; }

void InitializeContent() {
    if (initialized || !available) return;
    wchar_t exe[32768] = {};
    const auto length = GetModuleFileNameW(nullptr, exe, 32768);
    if (!length || length >= 32768) { error = ErrorCode::files_unavailable; return; }
    const std::wstring full(exe, length);
    const auto root = full.substr(0, full.find_last_of(L"\\/")) + L"\\";
    const wchar_t* roots[] = {L"patch_ae2_tu3",L"patch_ae2_tu2",L"patch_ae2_tu1b",L"patch_ae2_tu1",L"patch_ae2",L"patch",L"dlc\\04_ae2",L"dlc\\03_character_free",L"dlc\\03_ae",L"resource"};
    for (const auto& track : music::TrackList()) {
        paths[track.id] = track.path;
        const std::wstring relative(track.path.begin(), track.path.end());
        for (const auto folder : roots) if (Exists(root + folder + L"\\" + relative)) {
            installed[track.id] = true; installedTracks.push_back(track.id); break;
        }
    }
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    randomMusic.seed(static_cast<unsigned>(counter.QuadPart ^ GetTickCount64() ^ GetCurrentProcessId()));
    spdlog::info("Music: found {}/{} stock tracks", installedTracks.size(), music::TrackCount);
    initialized = true;
}
void Configure(int lobby, int match, bool safeMenu, bool inRoom) {
    lobby = music::Normalize(lobby, false); match = music::Normalize(match);
    if (lobby != lobbyChoice || match != configuredMatch) {
        if (available) error=ErrorCode::None;
        if (initialized && ((lobby != music::Default && InstalledChoice(lobby) == music::Default) ||
            (match != music::Default && match != music::Random && InstalledChoice(match) == music::Default)))
            error = ErrorCode::missing;
    }
    lobbyChoice = lobby; configuredMatch = match; safe = safeMenu; room = inRoom;
}
void EnterMatch() {
    // Independent of gameplay/room RNG. Never store the resolved track over
    // the Random preference, and never reroll on individual loader calls.
    const auto roll = installedTracks.empty() ? 0u :
        std::uniform_int_distribution<unsigned>(0, static_cast<unsigned>(installedTracks.size() - 1))(randomMusic);
    matchChoice = music::ResolveMatch(configuredMatch, roll, installedTracks);
    spdlog::info("Music: match preference={} resolved={}", configuredMatch, matchChoice.load());
    matchArmed = true; safe = false;
}
void LeaveMatch() { matchArmed = false; }
void RetryLobby() {
    if (loadFailed) { playback.Retry(); loadFailed = false; error=ErrorCode::None; }
}
std::string Error() {
    switch(error) {
    case ErrorCode::fallback: return loc::T("music.error_fallback");
    case ErrorCode::retry_default: return loc::T("music.error_retry_default");
    case ErrorCode::load_failed: return loc::T("music.error_load_failed");
    case ErrorCode::disabled: return loc::T("music.error_disabled");
    case ErrorCode::unsupported: return loc::T("music.error_unsupported");
    case ErrorCode::hook_failed: return loc::T("music.error_hook_failed");
    case ErrorCode::files_unavailable: return loc::T("music.error_files_unavailable");
    case ErrorCode::missing: return loc::T("music.error_missing");
    default: return {};
    }
}
} }
