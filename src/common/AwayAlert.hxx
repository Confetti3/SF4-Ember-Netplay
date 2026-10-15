#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// A call back from Training, or the opponent at the player's table readying,
// while the game's window is behind another or minimized. The game mutes
// itself there unless Play in the background is on, and its announcer only
// speaks at the main menu, so the player would miss both. Ember then flashes
// the window's taskbar button and plays a chime of its own through Windows,
// never through the game's mixer (sf4e__AwayAlert.cxx). In front nothing
// changes.
namespace sf4e { namespace away_alert {

// What an alert does now. Each call and each ready comes once from its own
// transition (TrainingCall's Call step, ReadyChime), so nothing here counts
// or merges them: only while the window is behind another or minimized does
// the taskbar flash, and the chime plays there with the ready sound on.
struct Plan { bool flash = false, chime = false; };
inline Plan PlanFor(bool inFront, bool minimized, bool sound) {
    Plan plan;
    plan.flash = minimized || !inFront;
    plan.chime = plan.flash && sound;
    return plan;
}

// The chime: two short rising tones, a 16-bit mono WAV file in memory for
// PlaySoundW, at volumePercent (10 to 100) of its full level.
inline std::vector<std::uint8_t> Chime(int volumePercent) {
    constexpr std::uint32_t Rate = 22050;
    struct Tone { double hz, seconds; };
    const Tone tones[] = { {880.0, .12}, {1318.5, .22} };
    const double level = 0.6 * 32767.0 * ((volumePercent < 10 ? 10 : volumePercent > 100 ? 100 : volumePercent) / 100.0);
    std::vector<std::int16_t> samples;
    for (const auto& tone : tones) {
        const auto count = static_cast<std::uint32_t>(tone.seconds * Rate);
        for (std::uint32_t i = 0; i < count; ++i) {
            const double t = static_cast<double>(i) / Rate;
            // A 5 ms rise, then a fall to silence by the tone's end.
            const double rise = t < .005 ? t / .005 : 1.0;
            const double fall = std::exp(-4.0 * t / tone.seconds) * (1.0 - static_cast<double>(i) / count);
            samples.push_back(static_cast<std::int16_t>(level * rise * fall * std::sin(2 * 3.14159265358979 * tone.hz * t)));
        }
    }
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    std::vector<std::uint8_t> wave(44 + dataBytes);
    auto* at = wave.data();
    const auto put = [&at](const void* value, std::size_t size) { std::memcpy(at, value, size); at += size; };
    const auto u32 = [&put](std::uint32_t value) { put(&value, 4); };
    const auto u16 = [&put](std::uint16_t value) { put(&value, 2); };
    put("RIFF", 4); u32(36 + dataBytes); put("WAVE", 4);
    put("fmt ", 4); u32(16); u16(1); u16(1); u32(Rate); u32(Rate * 2); u16(2); u16(16);
    put("data", 4); u32(dataBytes); put(samples.data(), dataBytes);
    return wave;
}

} }
