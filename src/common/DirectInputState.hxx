#pragma once
#include "GamepadSlots.hxx"
#include <cstring>
#include <map>

namespace sf4e { namespace gamepads {

struct NativeProfile { std::uint16_t vid, pid; };
// The game's native DirectInput profiles, from guidProduct strings at 0x517900.
constexpr std::array<NativeProfile, 14> NativeProfiles = {{
    {0x0079, 0x0011}, {0x05FD, 0x262A}, {0x046D, 0xC20C}, {0x0E8F, 0x0003},
    {0x0E8F, 0x3013}, {0x0F0D, 0x0032}, {0x0F0D, 0x0022}, {0x0738, 0x8838},
    {0x1A34, 0x0802}, {0x0E8F, 0x0012}, {0x0079, 0x0006}, {0x046D, 0xC216},
    {0x0079, 0x0002}, {0x0F0D, 0x0037}
}};

// SF4's rule for a pad it reads itself (Classify's GameReadsPad): a stick of
// one of its native DirectInput profiles. Steam's pad is XInput even when it
// reports a native stick's VID:PID.
inline bool ReadsNativeProfile(const Pad& pad) {
    return !SteamVirtual(pad) && std::any_of(NativeProfiles.begin(), NativeProfiles.end(), [&](const NativeProfile& profile) {
        return pad.vid == profile.vid && pad.pid == profile.pid;
    });
}

inline bool Served(std::uint32_t vidpid, const std::string& path, const std::vector<Pad>& inventory) {
    const auto owned = [](const Pad& pad) {
        return pad.available && pad.status != Status::Unmapped && pad.status != Status::GameRead;
    };
    const auto device = std::find_if(inventory.begin(), inventory.end(), [&](const Pad& pad) {
        return SameDriverPath(path, pad.identity.path);
    });
    if (device != inventory.end()) return owned(*device);
    // An uncertain identity must not hide a pad that SDL could not take over.
    bool matched = false;
    for (const auto& pad : inventory) if (vidpid == (std::uint32_t(pad.pid) << 16 | pad.vid)) {
        if (!owned(pad)) return false;
        matched = true;
    }
    return matched;
}

struct AxisRange {
    std::int32_t min = 0, max = 65535;
    std::int32_t Centre() const { return static_cast<std::int32_t>((std::int64_t(min) + max) / 2); }
};

// DirectInput's caller chooses the byte layout. Keep only the fields needed
// to produce neutral input, with resolved object ids for per-axis ranges.
class NeutralState {
public:
    enum class Kind { Axis, Pov };
    struct Field { std::uint32_t offset, id; Kind kind; };

    void SetFormat(std::uint32_t size, std::vector<Field> fields) {
        size_ = size;
        fields_ = std::move(fields);
    }
    void SetRange(AxisRange range) { default_ = range; ranges_.clear(); }
    void SetRange(std::uint32_t id, AxisRange range) { ranges_[id] = range; }

    bool Write(std::uint32_t size, void* out) const {
        if (!out || !size_ || size != size_) return false;
        auto* bytes = static_cast<unsigned char*>(out);
        std::memset(bytes, 0, size);
        for (const auto& field : fields_) {
            if (field.offset > size || size - field.offset < sizeof(std::uint32_t)) return false;
            const auto range = ranges_.find(field.id);
            const std::uint32_t value = field.kind == Kind::Pov ? 0xFFFFFFFFu :
                static_cast<std::uint32_t>((range == ranges_.end() ? default_ : range->second).Centre());
            std::memcpy(bytes + field.offset, &value, sizeof(value));
        }
        return true;
    }

private:
    std::uint32_t size_ = 0;
    std::vector<Field> fields_;
    AxisRange default_;
    std::map<std::uint32_t, AxisRange> ranges_;
};

} }
