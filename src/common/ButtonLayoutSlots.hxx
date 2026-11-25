#pragma once
#include "ButtonLayout.hxx"
#include "PadBindingRecords.hxx"

namespace sf4e { namespace gamepads {
inline bool ReadSlot(const PadBindingRecords& records, unsigned slot, ButtonLayout& layout) {
    const auto* table = records.FightTable(slot);
    if (!table) return false;
    for (unsigned i = 0; i < layout.size(); ++i) layout[i] = table[LayoutBits[i]];
    return true;
}
// The slot's table as Ember edits it (a layout's start). The game copy a restore needs is ReadSlot's, with every bit.
inline bool ReadFightSlot(const PadBindingRecords& records, unsigned slot, ButtonLayout& layout) {
    if (!ReadSlot(records, slot, layout)) return false;
    layout = FightFunctions(layout);
    return true;
}
inline bool WriteSlot(const PadBindingRecords& records, unsigned slot, const ButtonLayout& layout) {
    auto* table = records.FightTable(slot);
    if (!table || slot >= 4) return false;
    for (unsigned i = 0; i < layout.size(); ++i) table[LayoutBits[i]] = layout[i];
    return true;
}
struct LayoutPlacement { Identity identity; ButtonLayout buttons; std::uint32_t instance = 0; };
struct LayoutOwner { Identity identity; ButtonLayout buttons, gameCopy; std::uint32_t instance = 0; };
using LayoutOwners = std::array<std::optional<LayoutOwner>, 4>;
using LayoutPlacements = std::array<std::optional<LayoutPlacement>, 4>;
enum class LayoutSource { Layout, GameCopy };
struct SlotWrite { unsigned slot; LayoutSource source; bool copyFirst; };
inline std::vector<SlotWrite> SlotWritePlan(const LayoutOwners& owners, const LayoutPlacements& placements,
    const std::array<bool, 4>& occupied) {
    std::vector<SlotWrite> writes;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!occupied[slot]) continue;
        const auto& owner = owners[slot]; const auto& next = placements[slot];
        if (next && (!owner || !Same(owner->identity, next->identity) || owner->buttons != next->buttons || owner->instance != next->instance))
            writes.push_back({slot, LayoutSource::Layout, !owner});
        else if (!next && owner) writes.push_back({slot, LayoutSource::GameCopy, false});
    }
    return writes;
}
// Transactional bounds check: a bad vector cannot partially apply a plan.
inline bool ApplySlotWrites(LayoutOwners& owners, const LayoutPlacements& placements,
    const std::vector<SlotWrite>& writes, const PadBindingRecords& records) {
    for (const auto& write : writes) if (write.slot >= 4 || !records.FightTable(write.slot)) return false;
    for (const auto& write : writes) {
        auto& owner = owners[write.slot];
        if (write.source == LayoutSource::GameCopy) { WriteSlot(records, write.slot, owner->gameCopy); owner.reset(); }
        else {
            ButtonLayout copy;
            if (owner) copy = owner->gameCopy;
            else ReadSlot(records, write.slot, copy);
            const auto& next = *placements[write.slot];
            WriteSlot(records, write.slot, next.buttons);
            owner = LayoutOwner{next.identity, next.buttons, copy, next.instance};
        }
    }
    return true;
}
inline bool RestoreGameLayouts(LayoutOwners& owners, const PadBindingRecords& records) {
    std::vector<SlotWrite> writes;
    for (unsigned slot = 0; slot < owners.size(); ++slot)
        if (owners[slot]) writes.push_back({slot, LayoutSource::GameCopy, false});
    return ApplySlotWrites(owners, {}, writes, records);
}
// The two game paths that rewrite the binding records' tables after startup:
// its button screen (OptionUIController) and its .ctrlsav loader (0x51BC30,
// run on each visit to the title screen).
enum class GameTableUse { ButtonScreen, ConfigLoad };
// While one of those paths runs, the game works on its own tables: Open writes
// every owned slot's gameCopy back, and Close takes what the game left there
// as the new gameCopy, whatever it is, then writes the layouts again. This is
// the only way an owned slot's gameCopy changes after it is first saved.
class GameTableSwap {
public:
    // Refused while another use holds the tables; that use's Close reclaims them.
    bool Open(GameTableUse use, const LayoutOwners& owners, const PadBindingRecords& records) {
        if (use_ || !Owned(owners, records)) return false;
        for (unsigned slot = 0; slot < 4; ++slot) if (owners[slot]) WriteSlot(records, slot, owners[slot]->gameCopy);
        use_ = use;
        return true;
    }
    bool Close(LayoutOwners& owners, const PadBindingRecords& records) {
        if (!use_ || !Owned(owners, records)) return false;
        for (unsigned slot = 0; slot < 4; ++slot) if (auto& owner = owners[slot]) {
            ReadSlot(records, slot, owner->gameCopy);
            WriteSlot(records, slot, owner->buttons);
        }
        use_.reset();
        return true;
    }
    std::optional<GameTableUse> Use() const { return use_; }
private:
    // Transactional bounds check, as in ApplySlotWrites.
    static bool Owned(const LayoutOwners& owners, const PadBindingRecords& records) {
        for (unsigned slot = 0; slot < 4; ++slot) if (owners[slot] && !records.FightTable(slot)) return false;
        return true;
    }
    std::optional<GameTableUse> use_;
};
} }
