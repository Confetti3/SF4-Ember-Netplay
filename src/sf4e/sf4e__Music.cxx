#include "sf4e__Music.hxx"
#include "sf4e__MusicRematch.hxx"
#include "sf4e__CustomContent.hxx"
#include "../common/GameFiles.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/NativeSignature.hxx"
#include "../common/MusicPlaybackState.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/Localization.hxx"
#include <windows.h>
#include <intrin.h>
#include <detours/detours.h>
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
struct NativeHooks {
    CreateSheet createSheet = nullptr;
    BattleBgmConstructor battleBgmConstructor = nullptr;
    MenuMethod menuUpdate = nullptr, menuRelease = nullptr, menuLoad = nullptr;
    MenuReady menuReady = nullptr;
    MenuPlay menuPlay = nullptr;
    std::uintptr_t base = 0;
    bool attached = false;
    std::atomic<bool> available{false};
} hooks;
struct Inventory {
    bool ready = false;
    std::vector<int> tracks;
    music::ChoicesPtr choices = std::make_shared<const music::Choices>();
    std::mt19937 random;
    int Installed(int choice) const {
        return std::find(tracks.begin(), tracks.end(), choice) == tracks.end() ? music::Default : choice;
    }
    int Resolve(int preference) {
        return music::ResolveMatch(preference, random(), tracks);
    }
} inventory;
struct MenuBank {
    void* manager = nullptr;
    music::MenuPlaybackState playback;
    int preference = music::Default, loadingChoice = music::Default;
    ULONGLONG loadStarted = 0;
    bool safe = false, reloading = false, fallingBack = false, failed = false;
} menu;
struct BattleBank {
    int preference = music::Default, choice = music::Default, nextChoice = music::Default;
    music::RematchMusicState cycle;
} battle;
enum class ErrorCode { None, Fallback, RetryDefault, LoadFailed, Disabled, Unsupported, HookFailed, FilesUnavailable, Missing };
ErrorCode error = ErrorCode::None;

const char* Replacement(int choice, const char* original) {
    const auto* track = music::Find(choice);
    return track ? track->path.c_str() : original;
}
void* __cdecl Create(const char* path, unsigned flags, int streaming, int arg4, int arg5) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - hooks.base;
    // Only the primary UI background bank while this module reloads it.
    // No menu effects, character sounds, announcer or native fight state changes.
    if (hooks.available && path && menu.reloading && caller == 0x2869a7 &&
        std::strcmp(path, "ui/sound/bgm/BGM_MAIN.csb") == 0)
        path = Replacement(menu.loadingChoice, path);
    // The battle's BGM loader, not its SE/voice resource constructors.
    else if (hooks.available && path && battle.choice != music::Default && !custom::HasStageMusic() && caller == 0x1efedd &&
        std::strncmp(path, "battle/sound/bgm/BGM_", 21) == 0)
        path = Replacement(inventory.Installed(battle.choice), path);
    return hooks.createSheet(path, flags, streaming, arg4, arg5);
}
void* __fastcall ConstructBattleBgm(void* self, void*, const BattleBgmInit* init) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - hooks.base;
    const int choice = music::BattleChoice(inventory.Installed(battle.choice), custom::HasStageMusic());
    if (hooks.available && init && choice != music::Default && caller == 0x1928e8) {
        // The constructor copies both fields; it does not retain this pointer.
        // Let its native single-cue path ignore unsupported phase changes,
        // instead of crossfading Sakura's cue 0 into nonexistent cues 1/2.
        auto adjusted = *init;
        adjusted.mode = music::BattleCueMode(choice, init->mode);
        spdlog::info("Music: battle player track={} native_mode={} selected_mode={}", choice, init->mode, adjusted.mode);
        auto* player = hooks.battleBgmConstructor(self, &adjusted);
        rematch::TrackPlayer(player);
        return player;
    }
    auto* player = hooks.battleBgmConstructor(self, init);
    if (hooks.available && caller == 0x1928e8) rematch::TrackPlayer(player);
    return player;
}
void Reload(void* manager, int choice) {
    menu.reloading = true;
    menu.loadingChoice = choice;
    hooks.menuRelease(manager);
    hooks.menuLoad(manager);
    menu.reloading = false;
    menu.loadStarted = GetTickCount64();
}
void __fastcall Release(void* self, void*) {
    hooks.menuRelease(self);
    if (self == menu.manager) menu.playback.Reset();
}
void __fastcall Update(void* self, void*) {
    hooks.menuUpdate(self);
    if (self != menu.manager) { menu.manager = self; menu.playback.Reset(); }
    if (!hooks.available || !inventory.ready || !menu.safe) return;
    if (menu.playback.NeedsChoice(menu.preference, menu.safe) &&
        menu.playback.RequestChoice(menu.preference, inventory.random(), inventory.tracks, menu.safe)) {
        const int wanted = menu.playback.applied;
        menu.fallingBack = false;
        spdlog::info("Music: loading menu track {}", wanted);
        Reload(self, wanted);
    }
    if (!menu.playback.pending) return;
    if (hooks.menuReady(self)) {
        hooks.menuPlay(self, 0);
        menu.playback.pending = false;
        spdlog::info("Music: menu bank ready (track={}, fallback={})", menu.playback.applied, menu.fallingBack);
        if (menu.fallingBack) error = ErrorCode::Fallback;
    } else if (GetTickCount64() - menu.loadStarted >= 10000) {
        menu.failed = true;
        // Fall back once, without retrying a bad bank every frame. The original
        // game owns cleanup on its next scene change. Retry on a new selection.
        if (!menu.fallingBack) {
            error = ErrorCode::RetryDefault;
            menu.fallingBack = true; Reload(self, music::Default);
        } else {
            error = ErrorCode::LoadFailed;
            menu.playback.pending = false;
        }
    }
}
}

void Install() {
    if (EnvFlag("SF4E_DISABLE_MUSIC")) { error = ErrorCode::Disabled; return; }
    hooks.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Reject a different engine layout before attaching any of these hooks.
    const unsigned char create[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x3b,0xea,0x8f,0x00};
    const unsigned char update[] = {0x56,0x8b,0xf1,0x8d,0x4e,0x18,0xe8};
    const unsigned char release[] = {0x56,0x57,0x8b,0xf9,0xe8};
    const unsigned char load[] = {0x53,0x8b,0xd9,0x57,0x8d,0x4b,0x18};
    const unsigned char play[] = {0x55,0x8b,0xec,0x53,0x8b,0x5d,0x08,0x56};
    const unsigned char ready[] = {0x83,0xc1,0x18,0xe8};
    const unsigned char battleConstructor[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x38,0x29,0x90,0x00};
    if (!native::Signature(hooks.base, 0x275e40, create) || !native::Signature(hooks.base, 0x286940, update) || !native::Signature(hooks.base, 0x286620, release) ||
        !native::Signature(hooks.base, 0x2865b0, load) || !native::Signature(hooks.base, 0x286680, play) || !native::Signature(hooks.base, 0x286460, ready) ||
        !native::Signature(hooks.base, 0x192630, battleConstructor)) {
        error = ErrorCode::Unsupported; return;
    }
    hooks.createSheet = reinterpret_cast<CreateSheet>(hooks.base + 0x275e40);
    hooks.menuUpdate = reinterpret_cast<MenuMethod>(hooks.base + 0x286940);
    hooks.menuRelease = reinterpret_cast<MenuMethod>(hooks.base + 0x286620);
    hooks.menuLoad = reinterpret_cast<MenuMethod>(hooks.base + 0x2865b0);
    hooks.menuPlay = reinterpret_cast<MenuPlay>(hooks.base + 0x286680);
    hooks.menuReady = reinterpret_cast<MenuReady>(hooks.base + 0x286460);
    hooks.battleBgmConstructor = reinterpret_cast<BattleBgmConstructor>(hooks.base + 0x192630);
    if (!rematch::Install()) { error = ErrorCode::Unsupported; return; }
    // Sidecar owns transaction failure/rollback and does not launch on failure.
    const auto a = DetourAttach(reinterpret_cast<PVOID*>(&hooks.createSheet), reinterpret_cast<PVOID>(Create));
    const auto b = DetourAttach(reinterpret_cast<PVOID*>(&hooks.menuUpdate), reinterpret_cast<PVOID>(Update));
    const auto c = DetourAttach(reinterpret_cast<PVOID*>(&hooks.menuRelease), reinterpret_cast<PVOID>(Release));
    const auto d = DetourAttach(reinterpret_cast<PVOID*>(&hooks.battleBgmConstructor), reinterpret_cast<PVOID>(ConstructBattleBgm));
    hooks.attached = a == NO_ERROR && b == NO_ERROR && c == NO_ERROR && d == NO_ERROR;
    if (!hooks.attached) error = ErrorCode::HookFailed;
}
void Activate() { hooks.available = hooks.attached; }
bool Available() { return hooks.available; }

void InitializeContent() {
    if (inventory.ready || !hooks.available) return;
    const auto root = platform::GameDirectory();
    if (root.empty()) { error = ErrorCode::FilesUnavailable; return; }
    for (const auto& track : music::TrackList())
        if (!files::FindGameFile(root, track.path).empty()) inventory.tracks.push_back(track.id);
    inventory.choices = std::make_shared<const music::Choices>(inventory.tracks);
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    inventory.random.seed(static_cast<unsigned>(counter.QuadPart ^ GetTickCount64() ^ GetCurrentProcessId()));
    spdlog::info("Music: found {}/{} stock tracks", inventory.tracks.size(), music::TrackCount);
    inventory.ready = true;
}
music::ChoicesPtr InstalledChoices() { return inventory.choices; }
void Configure(int menuChoice, int matchChoice, bool safeMenu) {
    menuChoice = music::Normalize(menuChoice); matchChoice = music::Normalize(matchChoice);
    if (menuChoice != menu.preference || matchChoice != battle.preference) {
        if (hooks.available) error = ErrorCode::None;
        const auto missing = [](int choice) {
            return choice != music::Default && choice != music::Random && inventory.Installed(choice) == music::Default;
        };
        if (inventory.ready && (missing(menuChoice) || missing(matchChoice))) error = ErrorCode::Missing;
    }
    menu.preference = menuChoice; battle.preference = matchChoice; menu.safe = safeMenu;
    // A request abandoned before constructing an audio player has no native
    // destructor. Returning to a menu retires that pending audio selection too.
    if (safeMenu) ReleaseBattle();
}
int ResolveMatchChoice() {
    const int choice = hooks.available ? inventory.Resolve(battle.preference) : music::Default;
    spdlog::info("Music: match preference={} resolved={}", battle.preference, choice);
    return choice;
}
void TakeBattleChoice(int choice) {
    rematch::CancelPending();
    battle.choice = inventory.Installed(choice);
    battle.cycle = {};
    menu.safe = false;
}
void ReleaseBattle() {
    battle.choice = music::Default;
    battle.cycle = {};
    rematch::CancelPending();
}
void ObserveMatch(std::uint64_t confirmedGame, bool decided) {
    if (!hooks.available || battle.choice == music::Default || custom::HasStageMusic() ||
        battle.preference != music::Random || !confirmedGame || confirmedGame < battle.cycle.current) return;
    if (battle.cycle.Observe(confirmedGame, decided)) {
        battle.nextChoice = music::NextRandomTrack(battle.choice, inventory.random(), inventory.tracks);
        if (battle.nextChoice != music::Default && battle.nextChoice != battle.choice)
            rematch::Prepare(music::Find(battle.nextChoice)->path.c_str(), battle.nextChoice);
    }
    if (rematch::Tick(battle.cycle.MayApply()) == rematch::Progress::Applied)
        battle.choice = battle.nextChoice;
}
void RetryMenu() {
    if (menu.failed) { menu.playback.Retry(); menu.failed = false; error=ErrorCode::None; }
}
std::string Error() {
    switch(error) {
    case ErrorCode::Fallback: return loc::T("music.error_fallback");
    case ErrorCode::RetryDefault: return loc::T("music.error_retry_default");
    case ErrorCode::LoadFailed: return loc::T("music.error_load_failed");
    case ErrorCode::Disabled: return loc::T("music.error_disabled");
    case ErrorCode::Unsupported: return loc::T("music.error_unsupported");
    case ErrorCode::HookFailed: return loc::T("music.error_hook_failed");
    case ErrorCode::FilesUnavailable: return loc::T("music.error_files_unavailable");
    case ErrorCode::Missing: return loc::T("music.error_missing");
    default: return {};
    }
}
} }
