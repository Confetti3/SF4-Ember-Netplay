#include "sf4e__MusicRematch.hxx"
#include "../common/MusicPlaybackState.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Eva.hxx"
#include <windows.h>
#include <detours/detours.h>
#include <cstring>
#include <atomic>
#include <spdlog/spdlog.h>

namespace sf4e { namespace Music { namespace rematch {
namespace {
using System = Dimps::Game::Battle::System;
using Task = Dimps::Eva::Task;
using TaskCore = Dimps::Eva::TaskCore;
using CreateSheet = void* (__cdecl*)(const char*, unsigned, int, int, int);
using Method = void (__thiscall*)(void*);
using Poll = int (__thiscall*)(void*);
using Destroy = void* (__thiscall*)(void*, unsigned);
struct Init { unsigned sheet; int mode; };
struct Cue { unsigned handle; float mix; };
struct Player {
    std::uintptr_t vtable;
    unsigned sheet;
    Cue* cues;
    int selected, count, playing, ducked;
    float volume, fade;
};
static_assert(sizeof(Player) == 0x24, "Steam 1.05 BgmPlayer layout");
using Construct = void* (__thiscall*)(void*, const Init*);
using Start = void (__thiscall*)(void*, int);
std::uintptr_t base = 0;
CreateSheet createSheet = nullptr;
Method loadSheet = nullptr, clearPlayer = nullptr;
Poll pollSheet = nullptr;
Destroy destroySheet = nullptr, destroyPlayer = nullptr;
Construct constructPlayer = nullptr;
Start startPlayer = nullptr;
void* player = nullptr;
void* pendingSheet = nullptr;
void* activeSheet = nullptr;
int pendingChoice = music::Default;
ULONGLONG loadStarted = 0;
bool installed = false;
std::atomic<DWORD> ownerThread{0};
std::atomic<bool> cancelRequested{false};

bool Readable(const void* pointer, std::size_t size) {
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (!begin || !size || begin + size < begin) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(pointer, &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = region.Protect & 0xff;
    return (protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY) &&
        begin + size <= reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
}
template<std::size_t N> bool Signature(std::uintptr_t rva, const unsigned char (&bytes)[N]) {
    const auto* pointer = reinterpret_cast<const void*>(base + rva);
    return Readable(pointer, N) && std::memcmp(pointer, bytes, N) == 0;
}
void ReleaseSheet(void*& sheet) {
    if (sheet) { destroySheet(sheet, 1); sheet = nullptr; }
}
void* __fastcall DestroyPlayer(void* self, void*, unsigned flags) {
    // Its native cleanup stops/releases cue players before their sheet dies.
    // Do not tear down the sound unit, whose SE/voice managers own rollback data.
    const bool ours = self == player;
    auto* result = destroyPlayer(self, flags);
    if (ours) {
        player = nullptr;
        ReleaseSheet(pendingSheet);
        ReleaseSheet(activeSheet);
        ownerThread = 0;
        cancelRequested = false;
    }
    return result;
}
bool QuiescentPlayer() {
    if (!Readable(player, 0x24) || *static_cast<std::uintptr_t*>(player) != base + 0x551b98) return false;
    auto* system = System::staticMethods.GetSingleton();
    if (!Readable(system, sizeof(void*))) return false;
    auto* sound = (system->*System::publicMethods.GetUnitByIndex)(System::U_SOUND);
    if (!Readable(sound, 0xc) || *reinterpret_cast<void**>(static_cast<char*>(sound) + 8) != player) return false;
    auto* core = (system->*System::publicMethods.GetTaskCore)(System::TCI_UPDATE);
    if (!Readable(core, 0x4c)) return false;
    auto* task = TaskCore::GetTaskHead(core);
    for (unsigned count = 0; task && count < MAX_TASKS_PER_CORE; ++count) {
        if (!Readable(task, sizeof(Task))) return false;
        if (*Task::GetState(task) != Task::TS_CANCELLED) {
            auto* work = *Task::GetWorkFunctor(task);
            if (work) {
                if (!Readable(work, 0xc)) return false;
                const auto* fields = reinterpret_cast<const std::uintptr_t*>(work);
                if (fields[0] == base + 0x551b88 && fields[1] == reinterpret_cast<std::uintptr_t>(player))
                    return false;
            }
        }
        task = *Task::GetNext(task);
    }
    return task == nullptr;
}
}

bool Install() {
    base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const unsigned char create[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x3b,0xea,0x8f,0x00};
    const unsigned char load[] = {0xc7,0x41,0x0c,0x01,0x00,0x00,0x00,0xe9};
    const unsigned char poll[] = {0x55,0x8b,0xec,0x51,0x53,0x56,0x8b,0xf1};
    const unsigned char sheetDestroy[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x7b,0xdb,0x90,0x00};
    const unsigned char clear[] = {0x56,0x8b,0xf1,0x8b,0x06,0x8b,0x50,0x08,0x57,0x6a,0x01};
    const unsigned char construct[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x38,0x29,0x90,0x00};
    const unsigned char playerDestroy[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x68,0x29,0x90,0x00};
    const unsigned char start[] = {0x55,0x8b,0xec,0x56,0x8b,0xf1,0xb8,0x01,0x00,0x00,0x00};
    const unsigned char fade[] = {0x55,0x8b,0xec,0x51,0x53,0x8b,0xd9,0x8b,0x4d,0x08,0x56};
    const unsigned char makeFade[] = {0x55,0x8b,0xec,0x51,0xf3,0x0f,0x10,0x45,0x10};
    if (!Signature(0x275e40, create) || !Signature(0x275d60, load) || !Signature(0x275c10, poll) ||
        !Signature(0x275d70, sheetDestroy) || !Signature(0x192170, clear) ||
        !Signature(0x192630, construct) || !Signature(0x192740, playerDestroy) || !Signature(0x1922a0, start) ||
        !Signature(0x192520, fade) || !Signature(0x1927f0, makeFade))
        return false;
    createSheet = reinterpret_cast<CreateSheet>(base + 0x275e40);
    loadSheet = reinterpret_cast<Method>(base + 0x275d60);
    pollSheet = reinterpret_cast<Poll>(base + 0x275c10);
    destroySheet = reinterpret_cast<Destroy>(base + 0x275d70);
    clearPlayer = reinterpret_cast<Method>(base + 0x192170);
    constructPlayer = reinterpret_cast<Construct>(base + 0x192630);
    destroyPlayer = reinterpret_cast<Destroy>(base + 0x192740);
    startPlayer = reinterpret_cast<Start>(base + 0x1922a0);
    installed = DetourAttach(reinterpret_cast<PVOID*>(&destroyPlayer), reinterpret_cast<PVOID>(DestroyPlayer)) == NO_ERROR;
    return installed;
}

void TrackPlayer(void* value) {
    if (installed && !player) {
        player = value;
        ownerThread = GetCurrentThreadId();
    }
}
void CancelPending() {
    // Cancellation may accompany an outer lifecycle transition before the
    // native destructor. Defer native resource access to the known owner.
    cancelRequested = true;
    if (ownerThread && ownerThread == GetCurrentThreadId()) {
        ReleaseSheet(pendingSheet);
        cancelRequested = false;
    }
}
void Prepare(const char* path, int choice) {
    if (!installed || !ownerThread || ownerThread != GetCurrentThreadId()) return;
    CancelPending();
    pendingSheet = createSheet(path, 0x4001, 0, 0, 0);
    if (!pendingSheet) return;
    pendingChoice = choice;
    loadStarted = GetTickCount64();
    loadSheet(pendingSheet);
    spdlog::info("Music: preparing rematch track={}", choice);
}
Progress Tick(bool mayApply) {
    if (!ownerThread || ownerThread != GetCurrentThreadId()) return Progress::Waiting;
    if (cancelRequested.exchange(false)) ReleaseSheet(pendingSheet);
    if (!pendingSheet) return Progress::Waiting;
    const bool ready = pollSheet(pendingSheet) != 0;
    const auto status = *reinterpret_cast<const int*>(static_cast<const char*>(pendingSheet) + 4);
    const auto handle = *reinterpret_cast<const unsigned*>(static_cast<const char*>(pendingSheet) + 0x30);
    const auto invalid = *reinterpret_cast<const unsigned*>(base + 0x562620);
    if (status == -1 || (ready && handle == invalid) || (!ready && GetTickCount64() - loadStarted > 10000)) {
        CancelPending();
        spdlog::warn("Music: rematch bank failed; retaining the current track");
        return Progress::Failed;
    }
    if (!ready || !mayApply || !QuiescentPlayer()) return Progress::Waiting;
    // The only captured BGM functor holds this stable owner and volume-axis
    // 0/1, not a sheet/cue pointer. Native fades and crossfades read the current
    // cue array/count. The BgmPlayer itself is not a battle memento.
    // Rebuild only its native cue storage, preserving the owner address and
    // every unrelated sound manager. Never cancel saved scheduler tasks.
    const Init init{handle, music::BattleCueMode(pendingChoice, 1)};
    // Construct temporary native cue storage first. The constructor creates
    // no tasks and retains no pointer to this stack object. If its resource
    // handles are unavailable, the current player remains intact.
    Player replacement{};
    auto* created = constructPlayer(&replacement, &init);
    const int count = init.mode == 1 ? 3 : 1;
    bool valid = created == &replacement && replacement.vtable == base + 0x551b98 &&
        replacement.count == count && Readable(replacement.cues, count * sizeof(Cue));
    for (int i = 0; valid && i < count; ++i) valid = replacement.cues[i].handle != UINT32_MAX;
    if (!valid) {
        // A successful native constructor still owns its cue allocation,
        // even when the sound pool could not supply every handle.
        if (created == &replacement && replacement.vtable == base + 0x551b98) clearPlayer(&replacement);
        CancelPending();
        spdlog::warn("Music: rematch players unavailable; retaining the current track");
        return Progress::Failed;
    }
    clearPlayer(player);
    ReleaseSheet(activeSheet);
    *static_cast<Player*>(player) = replacement;
    activeSheet = pendingSheet;
    pendingSheet = nullptr;
    startPlayer(player, 0);
    spdlog::info("Music: rematch track={} applied with native cue mode={}", pendingChoice, init.mode);
    return Progress::Applied;
}
} } }
