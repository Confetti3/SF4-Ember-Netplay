#pragma once
#include "RoomLimits.hxx"
#include <cstdint>

namespace sf4e { namespace room {
// A table's set length: Unlimited, or first to N for N from 1 to
// MaxSetLength. Lengths without a name here are first to that many games.
enum class SetFormat : std::uint8_t { Ft1 = 1, Ft2 = 2, Ft3 = 3, Ft4 = 4, Ft5 = 5, Ft10 = 10, Unlimited = 0 };
// The longest set: first to 10 (best of 19). The bridge has the same limit
// (ember_protocol matches::MAX_GAMES_TO_WIN); keep them equal.
constexpr int MaxSetLength = 10;
inline bool ValidGamesToWin(long long games) { return games >= 1 && games <= MaxSetLength; }
inline bool ValidSetFormat(long long format) { return format == static_cast<int>(SetFormat::Unlimited) || ValidGamesToWin(format); }
enum class RotationMode : std::uint8_t { WinnerStays = 0, LoserStays, BothRotate };
struct Rules {
    SetFormat format = SetFormat::Unlimited;
    RotationMode rotation = RotationMode::WinnerStays;
    bool editionSelect = true;
    std::uint8_t roundCount = 3;
    std::uint16_t roundTime = 99;
    // Practice with another player: both fighters' health and gauges fill
    // again and nobody is knocked out. It changes the simulation, so it is
    // fixed with the other rules when the table prepares a game.
    bool training = false;
    bool operator==(const Rules& rhs) const {
        return format == rhs.format && rotation == rhs.rotation && editionSelect == rhs.editionSelect &&
            roundCount == rhs.roundCount && roundTime == rhs.roundTime && training == rhs.training;
    }
};
// A public room's tables start at first to 2, winner stays, so its queue
// moves without the host finding Table options first.
inline Rules PublicRoomRules() {
    Rules rules;
    rules.format = SetFormat::Ft2;
    rules.rotation = RotationMode::WinnerStays;
    return rules;
}
} }
