#pragma once
#include <string>
#include <vector>

namespace sf4e { namespace speech {
// Ember's own voice for its menus: SAPI 5, which every Windows has, or under
// Proton a Piper voice and speech-dispatcher, through a relay on the host
// (SpeechLinux.hxx). A screen reader through Tolk is the better voice for
// players who run one, and goes behind these same calls. None of them waits
// on the voice: it runs on a thread of its own, started by the first call.

// An utterance comes in parts (a row's name, its value); a voice that says a
// line whole gets them joined, and a part that ends its own sentence is not
// given a comma.
inline std::string Line(const std::vector<std::string>& parts) {
    std::string line;
    for (const auto& part : parts) line += (line.empty() ? "" : line.back() == '.' ? " " : ", ") + part;
    return line;
}
// interrupt drops whatever Ember is still saying first. rate is -10 to 10,
// volume 0 to 100. The voice speaks the interface's language where it can.
void Say(const std::vector<std::string>& parts, bool interrupt, int rate, int volume);
// Parts a screen that just opened may soon say, for a voice that keeps what
// it has said to make ahead; others let them go.
void Render(const std::vector<std::string>& parts, int rate);
void Stop();
// The notice chime: a short falling pair of tones, at volume percent, through
// Windows (winmm) under Proton too. It does not wait.
void Chime(int volume);
// What kept speech from being heard, if anything: Windows has no SAPI voice,
// or under Wine speech-dispatcher could not be reached. It stays so for the
// session.
enum class Missing { Nothing, WindowsVoice, SpeechDispatcher };
Missing Unavailable();
// Under Proton: whether the relay speaks with the system's voice, is
// downloading the natural (Piper) voice for the language, speaks with it, or
// could not download it. Windows stays System.
enum class Voice { System, Downloading, Natural, Failed };
struct VoiceStatus { Voice voice = Voice::System; int percent = 0; };
VoiceStatus NaturalVoice();
} }
