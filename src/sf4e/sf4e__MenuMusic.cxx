#include <windows.h>

#include "spdlog/spdlog.h"

#include "sf4e.hxx"
#include "sf4e__MenuMusic.hxx"

namespace {
// Steam 1.05: the menu reads its music's path from this entry (RVA 0x66b6f0), which holds
// "ui/sound/bgm/BGM_MAIN.csb".
constexpr std::uintptr_t entryRva = 0x66b6f0;

sf4e::MenuMusic::Target GameTarget() {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    sf4e::MenuMusic::Target target = {nullptr, "ui/sound/bgm/BGM_MAIN.csb", base, base};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return target;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return target;
    target.imageEnd = base + nt->OptionalHeader.SizeOfImage;
    MEMORY_BASIC_INFORMATION page = {};
    const std::uintptr_t at = base + entryRva;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (at + sizeof(const char*) <= target.imageEnd && VirtualQuery(reinterpret_cast<void*>(at), &page, sizeof(page)) && (page.Protect & writable))
        target.entry = reinterpret_cast<const char**>(at);
    return target;
}

// Made, and so checked, the first time it is used.
struct GameEntry : sf4e::MenuMusic::Entry {
    GameEntry() : Entry(GameTarget()) {
        if (const char* why = Unavailable()) spdlog::info("Menu music: endless lobby music is unavailable, {}", why);
    }
};
GameEntry& Game() {
    static GameEntry entry;
    return entry;
}
}

// The rolls are the game's local random numbers, which nothing else has to agree with.
void sf4e::MenuMusic::Use(bool endless) { Game().Use(endless, static_cast<unsigned>(sf4e::localRand())); }
void sf4e::MenuMusic::Pick() { Game().Pick(static_cast<unsigned>(sf4e::localRand())); }
bool sf4e::MenuMusic::Available() { return !Game().Unavailable(); }
