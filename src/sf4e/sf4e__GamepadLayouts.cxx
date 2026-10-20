#include "sf4e__GamepadLayouts.hxx"
#include "../common/ButtonLayoutJson.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../netplay/SettingsStore.hxx"
#include "sf4e__NetplayFacade.hxx"
#include <spdlog/spdlog.h>

namespace sf4e { namespace GamepadLayouts {
using namespace gamepads;
namespace {
std::vector<StoredLayout> layouts;
LayoutOwners owners;
GameTableSwap gameTables;
// A Close refused by the bounds check is retried on each update.
bool returningTables = false;
struct LastSlot { Identity identity; int slot; };
std::vector<LastSlot> lastSlots;
bool loggingReady = false;
std::vector<std::pair<spdlog::level::level_enum, std::string>> pendingLogs;
void Log(spdlog::level::level_enum level, std::string message) {
    if (loggingReady) spdlog::log(level, message);
    else pendingLogs.emplace_back(level, std::move(message));
}
std::vector<Identity> Connected(const Inventory& inventory) {
    std::vector<Identity> identities;
    for (const auto& pad : inventory.pads) identities.push_back(pad.identity);
    return identities;
}
}
void FlushLogs() {
    loggingReady = true;
    for (const auto& message : pendingLogs) spdlog::log(message.first, message.second);
    pendingLogs.clear();
}
void Load() {
    nlohmann::json values; std::string error;
    netplay::SettingsStore store(netplay::SettingsStore::DefaultDirectory());
    if (store.LoadLauncher(values, error)) layouts = ReadLayouts(values, [](unsigned index, LayoutLoadError error) {
        if (error.reason == LayoutLoadError::Reason::Mask)
            Log(spdlog::level::warn, fmt::format("Input: button layout {} rejected: mask 0x{:X} outside 0xCF0", index, error.mask));
        else Log(spdlog::level::warn, fmt::format("Input: button layout {} rejected: {}", index,
            error.reason == LayoutLoadError::Reason::Length ? "wrong length" : "mask is not an integer"));
    });
}
void Started(const Inventory& inventory, bool active) {
    if (!active) { Log(spdlog::level::info, fmt::format("Input: {} button layouts kept, not applied (the game reads controllers)", layouts.size())); return; }
    const auto connected = Connected(inventory);
    const auto absent = std::count_if(layouts.begin(), layouts.end(), [&](const StoredLayout& layout) {
        return std::none_of(connected.begin(), connected.end(), [&](const Identity& pad) { return Same(layout.identity, pad); });
    });
    Log(spdlog::level::info, fmt::format("Input: {} button layouts loaded, {} for controllers not connected", layouts.size(), absent));
}
void Set(std::vector<StoredLayout> next) { layouts = std::move(next); }
const std::vector<StoredLayout>& Stored() { return layouts; }
void Update(const Inventory& inventory) {
    if (returningTables) ReturnGameTables(*gameTables.Use());
    if (gameTables.Use()) return;
    const auto records = Dimps::Pad::BindingRecords();
    LayoutPlacements placements;
    std::array<bool, 4> occupied{};
    std::array<const Pad*, 4> pads{};
    const auto connected = Connected(inventory);
    for (const auto& pad : inventory.pads) if (pad.slot >= 0 && pad.slot < 4 && pad.status == Status::Slotted) {
        occupied[pad.slot] = true; pads[pad.slot] = &pad;
        const auto match = FindLayout(layouts, pad.identity, connected);
        if (!match.index) continue;
        auto& layout = layouts[*match.index];
        if (match.byGuid) {
            layout.identity.path = pad.identity.path;
            Log(spdlog::level::info, fmt::format("Input: layout for guid {} matched '{}' id {} by guid only; path updated",
                pad.identity.guid, pad.identity.name, pad.id));
        }
        placements[pad.slot] = LayoutPlacement{pad.identity, layout.buttons, pad.id};
    }
    const auto writes = SlotWritePlan(owners, placements, occupied);
    const auto previous = owners;
    if (!ApplySlotWrites(owners, placements, writes, records)) return;
    for (const auto& write : writes) {
        const auto& pad = *pads[write.slot];
        if (write.copyFirst) Log(spdlog::level::info, fmt::format("Input: slot {} game table saved before the first Ember layout", write.slot + 1));
        if (write.source == LayoutSource::GameCopy) {
            if (previous[write.slot] && Same(previous[write.slot]->identity, pad.identity))
                Log(spdlog::level::info, fmt::format("Input: slot {} back to the game's table (layout removed)", write.slot + 1));
            else Log(spdlog::level::info, fmt::format("Input: slot {} back to the game's table ('{}' id {} has no layout)", write.slot + 1, pad.identity.name, pad.id));
        }
        else {
            Log(spdlog::level::info, fmt::format("Input: layout for '{}' id {} applied to slot {}: {}", pad.identity.name, pad.id,
                write.slot + 1, LayoutDescription(placements[write.slot]->buttons)));
            const auto old = std::find_if(lastSlots.begin(), lastSlots.end(), [&](const LastSlot& placed) { return Same(placed.identity, pad.identity); });
            if (old != lastSlots.end() && old->slot != static_cast<int>(write.slot))
                Log(spdlog::level::info, fmt::format("Input: '{}' id {} moved from slot {} to slot {}; layout written to slot {}{}",
                    pad.identity.name, pad.id, old->slot + 1, write.slot + 1, write.slot + 1,
                    netplay::Fighting(NetplayFacade::GetRuntimeSnapshotShared()->session.match) ? ", during a match" : ""));
        }
    }
    for (const auto& pad : inventory.pads) if (pad.slot >= 0) {
        const auto old = std::find_if(lastSlots.begin(), lastSlots.end(), [&](const LastSlot& placed) { return Same(placed.identity, pad.identity); });
        if (old == lastSlots.end()) lastSlots.push_back({pad.identity, pad.slot});
        else *old = {pad.identity, pad.slot};
    }
}
void Read(PadFrame& frame, const Inventory& inventory) {
    const auto match = FindLayout(layouts, frame.pad.identity, Connected(inventory));
    if (match.index) { frame.layout = layouts[*match.index].buttons; frame.menu = layouts[*match.index].menu; frame.extras = layouts[*match.index].extras; }
    frame.table = frame.layout.value_or(DefaultLayout);
    if (frame.pad.slot >= 0) ReadSlot(Dimps::Pad::BindingRecords(), frame.pad.slot, frame.table);
}
void LendGameTables(GameTableUse use) {
    if (gameTables.Open(use, owners, Dimps::Pad::BindingRecords()) && use == GameTableUse::ButtonScreen)
        Log(spdlog::level::info, "Input: game button screen open; Ember layouts set aside");
}
void ReturnGameTables(GameTableUse use) {
    if (gameTables.Use() != use) return;
    returningTables = !gameTables.Close(owners, Dimps::Pad::BindingRecords());
    if (!returningTables) Log(spdlog::level::info, use == GameTableUse::ButtonScreen ?
        "Input: game button screen closed; Ember layouts restored" : "Input: game button config loaded; Ember layouts reapplied");
}
void Shutdown() {
    if (!RestoreGameLayouts(owners, Dimps::Pad::BindingRecords()))
        Log(spdlog::level::warn, "Input: game button tables could not be restored at shutdown (invalid binding records)");
    gameTables = {};
    returningTables = false;
}
} }
