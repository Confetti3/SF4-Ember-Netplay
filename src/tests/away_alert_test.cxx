// A call back from Training or a ready challenge is told outside the game
// only while its window is behind another or minimized: the taskbar flashes,
// and the chime plays with the ready sound on. Each comes once from its own
// transition (TrainingCall, ReadyChime), so nothing here merges them. The
// chime is a well-formed WAV at the chosen level.
#include "../common/AwayAlert.hxx"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "test_support.hxx"

namespace alert = sf4e::away_alert;

static void CheckOnlyAway() {
    for (const bool sound : {false, true}) {
        const auto front = alert::PlanFor(true, false, sound);
        CHECK(!front.flash && !front.chime);
        const auto behind = alert::PlanFor(false, false, sound);
        CHECK(behind.flash && behind.chime == sound);
        // Minimized, the window can still be the foreground one.
        const auto minimized = alert::PlanFor(true, true, sound);
        CHECK(minimized.flash && minimized.chime == sound);
    }
}

static std::uint32_t U32(const std::vector<std::uint8_t>& wave, std::size_t at) {
    std::uint32_t value; std::memcpy(&value, wave.data() + at, 4); return value;
}

static int Peak(const std::vector<std::uint8_t>& wave) {
    int peak = 0;
    for (std::size_t at = 44; at + 1 < wave.size(); at += 2) {
        std::int16_t sample; std::memcpy(&sample, wave.data() + at, 2);
        peak = (std::max)(peak, std::abs(static_cast<int>(sample)));
    }
    return peak;
}

static void CheckChime() {
    const auto full = alert::Chime(100), quiet = alert::Chime(10);
    CHECK(full.size() > 44 && std::memcmp(full.data(), "RIFF", 4) == 0 && std::memcmp(full.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(U32(full, 4) == full.size() - 8 && U32(full, 40) == full.size() - 44 && U32(full, 24) == 22050);
    CHECK(std::memcmp(full.data() + 36, "data", 4) == 0);
    const int loud = Peak(full), soft = Peak(quiet);
    CHECK(loud > 15000 && loud < 32767 && soft > 0 && soft * 8 < loud);
    // Out of range levels are held to it.
    CHECK(Peak(alert::Chime(500)) == loud && Peak(alert::Chime(0)) == soft);
}

int main() {
    CheckOnlyAway();
    CheckChime();
    return 0;
}
