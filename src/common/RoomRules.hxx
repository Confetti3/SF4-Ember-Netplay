#pragma once
#include "RoomLimits.hxx"
#include <cstdint>

namespace sf4e { namespace room {
enum class SetFormat : std::uint8_t { Ft1 = 1, Ft2 = 2, Ft3 = 3, Ft5 = 5, Unlimited = 0 };
enum class RotationMode : std::uint8_t { WinnerStays = 0, LoserStays, BothRotate };
struct Rules {
    SetFormat format = SetFormat::Unlimited;
    RotationMode rotation = RotationMode::WinnerStays;
    bool editionSelect = true;
    std::uint8_t roundCount = 3;
    std::uint16_t roundTime = 99;
    bool operator==(const Rules& rhs) const {
        return format == rhs.format && rotation == rhs.rotation && editionSelect == rhs.editionSelect &&
            roundCount == rhs.roundCount && roundTime == rhs.roundTime;
    }
};
} }
