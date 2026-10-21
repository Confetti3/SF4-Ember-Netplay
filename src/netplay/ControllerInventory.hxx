#pragma once
#include "InputAssignment.hxx"
#include "../common/ControllerView.hxx"
#include <optional>

namespace sf4e { namespace gamepads {
// Only the game thread builds rows. Retained identities keep pad-N stable
// through disconnects, slot changes, and the saved-controller placeholder.
class ControllerInventory {
public:
    std::vector<PadView> Build(Layer layer, const std::vector<PadFrame>& frames,
        const std::vector<input::Device>& devices, const std::vector<NativeController>& native,
        const input::Device& gameplay, const std::optional<Identity>& saved) {
        std::vector<PadView> result;
        if (layer == Layer::Active) {
            std::vector<int> usedNative;
            for (const auto& frame : frames) {
                const auto& pad = frame.pad;
                if (pad.status == Status::SteamTwin || pad.steamPad) continue;
                PadView row;
                row.identity = pad.identity; row.name = pad.identity.name;
                row.connectionOrder = pad.id;
                row.brand = DetectBrand(pad); row.artVariant = pad.type;
                row.picture = ControllerPicture(pad); row.slot = pad.slot;
                row.customMapping = pad.hasEmberMapping;
                row.state = pad.status == Status::Slotted ? (SteamVirtual(pad) ? RowState::Steam : RowState::Recognised) :
                    pad.status == Status::Unmapped ? RowState::Unmapped : pad.status == Status::GameRead ? RowState::GameRead : RowState::NotInUse;
                if (row.state == RowState::Unmapped) row.picture = "controller_generic";
                if (SteamVirtual(pad)) {
                    const auto twin = std::find_if(frames.begin(), frames.end(), [&](const PadFrame& candidate) { return candidate.pad.steamPad == pad.id; });
                    if (twin != frames.end()) row.name = twin->pad.identity.name;
                    else if (pad.type == Type::Unknown || pad.type == Type::Standard) row.name.clear(); // localized by the runtime
                }
                row.bindings = frame.bindings; row.numberedHats = frame.numberedHats;
                row.has = frame.has; row.table = frame.table; row.layout = frame.layout; row.menu = frame.menu;
                row.hasExtras = frame.hasExtras; row.extras = frame.extras; row.training = frame.training;
                row.held = HeldGlyphs(frame.state); row.raw = frame.raw; row.rawAtRest = frame.rawAtRest; row.pressed = frame.pressed;
                if (row.state == RowState::Unmapped || row.state == RowState::GameRead) {
                    const auto match = [&](const NativeController& n) {
                        return std::find(usedNative.begin(), usedNative.end(), n.index) == usedNative.end() &&
                            SameDriverPath(n.path, pad.identity.path);
                    };
                    const auto n = std::find_if(native.begin(), native.end(), match);
                    if (n != native.end()) {
                        row.directInputIndex = n->index; usedNative.push_back(n->index);
                        // Only a matching native name and index identifies gameplay input.
                        for (const auto& d : devices) if (d.connected && d.type == input::PadDirectInput && d.index == n->index &&
                            (d.name == n->instanceName || d.name == n->productName)) {
                            row.gameplay = gameplay.Same(d); row.pressed = d.buttons != 0 || row.pressed; break;
                        }
                    }
                } else row.gameplay = gameplay.type == input::PadXInput && !gameplay.identity.guid.empty() && Same(gameplay.identity, pad.identity);
                row.id = Number(row.identity);
                remembered_[row.id - 1] = row;
                result.push_back(std::move(row));
            }
        } else for (const auto& device : devices) if (device.connected && device.type != input::PadKeyboard) {
            PadView row; row.identity = device.identity;
            if (row.identity.guid.empty()) row.identity.path = std::to_string(device.type) + ":" + std::to_string(device.index) + ":" + device.name;
            row.identity.name = row.name = device.name;
            row.id = Number(row.identity); row.state = RowState::GameRead;
            row.slot = device.type == input::PadXInput ? device.index : -1;
            row.directInputIndex = device.index; row.gameplay = gameplay.Same(device);
            row.brand = GameplayBrand(device.type);
            row.picture = device.type == input::PadXInput ? "controller_xboxseries" : "controller_generic";
            row.pressed = device.buttons != 0;
            remembered_[row.id - 1] = row;
            result.push_back(std::move(row));
        }
        if (layer == Layer::Active && saved && std::none_of(result.begin(), result.end(), [&](const PadView& row) { return Same(row.identity, *saved); })) {
            const auto id = Number(*saved);
            auto row = remembered_[id - 1]; row.id = id; row.identity = *saved; row.name = saved->name;
            row.picture = "controller_disconnected"; row.state = RowState::Absent; row.slot = -1;
            row.held.clear(); row.raw.clear(); row.pressed = false;
            row.gameplay = gameplay.type == input::PadXInput && Same(gameplay.identity, *saved);
            result.push_back(std::move(row));
        }
        OrderPadRows(result);
        return result;
    }
private:
    unsigned Number(const Identity& identity) {
        for (unsigned i = 0; i < remembered_.size(); ++i) if (Same(remembered_[i].identity, identity)) return i + 1;
        PadView row; row.identity = identity; row.id = static_cast<unsigned>(remembered_.size()) + 1;
        remembered_.push_back(std::move(row));
        return static_cast<unsigned>(remembered_.size());
    }
    std::vector<PadView> remembered_;
};
} }
