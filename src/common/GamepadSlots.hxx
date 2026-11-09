#pragma once
#include "GamepadIdentity.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace sf4e { namespace gamepads {

using Id = std::uint32_t;
enum class Type { Unknown, Standard, Xbox360, XboxOne, PS3, PS4, PS5, SwitchPro,
    JoyConLeft, JoyConRight, JoyConPair, GameCube };
enum class Status { Slotted, Unmapped, NotInUse, SteamTwin, GameRead };

// SDL's name for a type (SDL_GetGamepadStringForType's).
inline const char* TypeName(Type type) {
    switch (type) {
    case Type::Standard: return "standard";
    case Type::Xbox360: return "xbox360";
    case Type::XboxOne: return "xboxone";
    case Type::PS3: return "ps3";
    case Type::PS4: return "ps4";
    case Type::PS5: return "ps5";
    case Type::SwitchPro: return "switchpro";
    case Type::JoyConLeft: return "joyconleft";
    case Type::JoyConRight: return "joyconright";
    case Type::JoyConPair: return "joyconpair";
    case Type::GameCube: return "gamecube";
    default: return "unknown";
    }
}

struct Pad {
    Id id = 0;
    Identity identity;
    std::uint16_t vid = 0, pid = 0;
    Type type = Type::Unknown;
    // SDL maps every button the setup wizard requires, or Ember's own setup maps the pad (MapsRequiredSteps).
    bool mapped = false;
    bool hasEmberMapping = false;
    // SDL maps the pad without an Ember setup (an Ember mapping hides SDL's own).
    bool sdlMapping = false;
    bool arcadeStick = false;
    bool available = true;
    int slot = -1;
    Status status = Status::NotInUse;
    Id steamPad = 0;
    bool operator==(const Pad& other) const {
        return std::tie(id, identity, vid, pid, type, mapped, hasEmberMapping, sdlMapping, arcadeStick, available, slot, status, steamPad) ==
            std::tie(other.id, other.identity, other.vid, other.pid, other.type, other.mapped,
                other.hasEmberMapping, other.sdlMapping, other.arcadeStick, other.available, other.slot, other.status, other.steamPad);
    }
};

struct Inventory {
    std::vector<Pad> pads;
    bool operator==(const Inventory& other) const { return pads == other.pads; }
};

// The GUID keeps Steam's own IDs; vid and pid name the physical pad when Steam supplies it.
inline bool SteamVirtual(const Pad& pad) {
    const auto ids = GuidVendorProduct(pad.identity.guid);
    return ids.vendor == 0x28DE && ids.product == 0x11FF;
}
// Only Ember's setup maps it, so its buttons have no printed names: it draws as a stick.
inline bool SetUpOnlyByEmber(const Pad& pad) { return pad.hasEmberMapping && !pad.sdlMapping; }

// The game's own rule for a pad it reads without the layer, which the layer
// therefore leaves alone (GameRead). SF4 Ember's: a stick of one of the
// game's DirectInput profiles (DirectInputState.hxx). SFV Ember's: an XInput
// device, or Steam's virtual pad (sfv-ember's mod/PadLayer.hxx).
using GameReadsPad = bool (*)(const Pad& pad);

// Also used before SDL opens handles, so Steam twins and pads the game reads are never opened.
// A pad Ember's own setup maps is the layer's even when the game would read it.
inline void Classify(std::vector<Pad>& pads, const std::set<std::string>& savedEmberMappingGuids, GameReadsPad gameReads) {
    for (auto& pad : pads) {
        pad.slot = -1;
        pad.steamPad = 0;
        pad.hasEmberMapping = savedEmberMappingGuids.count(pad.identity.guid) != 0;
        pad.status = gameReads(pad) && !pad.hasEmberMapping ? Status::GameRead :
            pad.mapped ? Status::NotInUse : Status::Unmapped;
    }
    for (const auto& steam : pads) if (SteamVirtual(steam)) {
        for (auto& physical : pads) if (!SteamVirtual(physical) && !physical.steamPad && physical.type == steam.type) {
            physical.steamPad = steam.id;
            if (physical.status != Status::GameRead) physical.status = Status::SteamTwin;
            break;
        }
    }
}

// SDL's DirectInput backend and the game both open a pad for exclusive use, so
// only one can read it. SDL holds an unmapped or game-read pad raw only while the
// setup wizard reads it (setup = the pad being set up); otherwise the game keeps it.
inline bool WantsRawHandle(const Pad& pad, const std::optional<Identity>& setup) {
    return setup && Same(pad.identity, *setup) && (pad.status == Status::Unmapped || pad.status == Status::GameRead);
}

struct DroppedReservation {
    int slot;
    Identity owner;
    Id replacement;
};

// Which pad holds which of the four XInput slots. A pad keeps its slot while
// it stays connected; a pad plugged in again takes back the slot it last held
// when that is free; any other takes the lowest free slot no pad last held,
// then any free slot.
class Slots {
public:
    explicit Slots(GameReadsPad rule) : gameReads(rule) {}

    // occupied: bit i when slot i answers without the layer (SFV Ember: a real
    // XInput pad or a virtual test pad), so no pad is given it.
    Inventory Update(std::vector<Pad> pads, const std::optional<Identity>& preferred = {},
        const std::set<std::string>& savedEmberMappingGuids = {}, unsigned occupied = 0) {
        Classify(pads, savedEmberMappingGuids, gameReads);
        dropped.clear();
        std::array<Id, 4> next{};
        const auto open = [&](int slot) { return !next[slot] && !((occupied >> slot) & 1u); };
        const auto eligible = [](const Pad& pad) { return pad.status == Status::NotInUse && pad.available; };
        const auto assign = [&](Pad& pad, int slot) {
            if (reserved[slot] && !Same(*reserved[slot], pad.identity))
                dropped.push_back({slot, *reserved[slot], pad.id});
            for (auto& owner : reserved) if (owner && Same(*owner, pad.identity)) owner.reset();
            reserved[slot] = pad.identity;
            next[slot] = pad.id;
            pad.slot = slot;
            pad.status = Status::Slotted;
        };
        // The saved pad takes slot 0 at layer start to retain the game's saved slot-based layout.
        if (startup && preferred && open(0))
            for (auto& pad : pads) if (eligible(pad) && Same(pad.identity, *preferred)) { assign(pad, 0); break; }
        for (int slot = 0; slot < 4; ++slot) if (open(slot))
            for (auto& pad : pads) if (eligible(pad) && pad.id == current[slot]) { assign(pad, slot); break; }
        std::vector<Pad*> waiting;
        for (auto& pad : pads) if (eligible(pad)) waiting.push_back(&pad);
        // Startup uses SDL enumeration order; later waiting pads use increasing connection ids.
        if (!startup) std::sort(waiting.begin(), waiting.end(), [](const Pad* a, const Pad* b) { return a->id < b->id; });
        for (auto* pad : waiting) {
            int free = -1;
            for (int slot = 0; slot < 4; ++slot)
                if (open(slot) && reserved[slot] && Same(*reserved[slot], pad->identity)) { free = slot; break; }
            if (free < 0)
                for (int slot = 0; slot < 4; ++slot) if (open(slot) && !reserved[slot]) { free = slot; break; }
            if (free < 0)
                for (int slot = 0; slot < 4; ++slot) if (open(slot)) { free = slot; break; }
            if (free >= 0) assign(*pad, free);
        }
        changed = next != current;
        current = next;
        startup = false;
        return {std::move(pads)};
    }

    const std::vector<DroppedReservation>& DroppedReservations() const { return dropped; }
    // Whether the last Update gave any slot another pad, or none.
    bool Changed() const { return changed; }

private:
    GameReadsPad gameReads;
    std::array<Id, 4> current{};
    std::array<std::optional<Identity>, 4> reserved;
    std::vector<DroppedReservation> dropped;
    bool changed = false;
    bool startup = true;
};

} }
