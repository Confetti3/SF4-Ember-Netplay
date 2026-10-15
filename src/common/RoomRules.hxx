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
// The round times a table offers, shortest first. The longest, about 2.7
// hours, is also every game's in a Practice room (room::Snapshot::practice),
// whose games end only when the time runs out.
constexpr std::uint16_t RoundTimes[] = {30, 60, 99, 300, 9999};
constexpr std::uint16_t DefaultRoundTime = 99;
constexpr std::uint16_t PracticeRoundTime = 9999;
inline bool ValidRoundTime(long long time) {
    for (const auto offered : RoundTimes) if (time == offered) return true;
    return false;
}
// The round counts a table offers; 99 plays on with no end.
constexpr std::uint8_t RoundCounts[] = {1, 3, 5, 7, 15, 99};
inline bool ValidRoundCount(long long count) {
    for (const auto offered : RoundCounts) if (count == offered) return true;
    return false;
}
struct Rules {
    SetFormat format = SetFormat::Unlimited;
    RotationMode rotation = RotationMode::WinnerStays;
    bool editionSelect = true;
    std::uint8_t roundCount = 3;
    std::uint16_t roundTime = DefaultRoundTime;
    // The one test of a set of rules, for the room that applies them, the
    // wire and saved room defaults that read them, and the settings that
    // write them.
    bool Valid() const {
        return ValidSetFormat(static_cast<int>(format)) && rotation <= RotationMode::BothRotate &&
            ValidRoundCount(roundCount) && ValidRoundTime(roundTime);
    }
    bool operator==(const Rules& rhs) const {
        return format == rhs.format && rotation == rhs.rotation && editionSelect == rhs.editionSelect &&
            roundCount == rhs.roundCount && roundTime == rhs.roundTime;
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
