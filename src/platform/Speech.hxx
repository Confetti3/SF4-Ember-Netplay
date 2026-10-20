#pragma once
#include <string>

namespace sf4e { namespace speech {
// Ember's own voice for its menus: SAPI 5, which every Windows has. A screen
// reader through Tolk is the better voice for players who run one, and goes
// behind these same two calls. Neither call waits on the voice: it runs on a
// thread of its own, started by the first Say.
// interrupt drops whatever is still being said first. rate is -10 to 10,
// volume 0 to 100.
void Say(const std::string& text, bool interrupt, int rate, int volume);
void Stop();
} }
