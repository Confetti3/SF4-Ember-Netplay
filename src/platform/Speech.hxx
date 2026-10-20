#pragma once
#include <string>

namespace sf4e { namespace speech {
// Ember's own voice for its menus: SAPI 5, which every Windows has, or under
// Proton speech-dispatcher, which a Steam Deck has (SpeechLinux.hxx). A screen
// reader through Tolk is the better voice for players who run one, and goes
// behind these same calls. Neither Say nor Stop waits on the voice: it runs
// on a thread of its own, started by the first call.
// interrupt drops whatever is still being said first. rate is -10 to 10,
// volume 0 to 100. The voice speaks the interface's language where it can.
void Say(const std::string& text, bool interrupt, int rate, int volume);
void Stop();
// What kept speech from being heard, if anything: Windows has no SAPI voice,
// or under Wine speech-dispatcher could not be reached. It stays so for the
// session.
enum class Missing { Nothing, WindowsVoice, SpeechDispatcher };
Missing Unavailable();
} }
