#pragma once
// The room chat as the player's chat filter shows it (PlayerPreferences::
// chatFilter, on unless they turn it off). Only this player's screen changes:
// the room keeps and sends every message as it was written.
#include <string>

namespace sf4e { namespace ui {
// `text` with each listed word hidden as one star per character, spaces
// kept. The lists are LDNOOBW's, less Ember's allow list of everyday words,
// plus Ember's stems (assets/chat-filter). In spaced languages an entry counts
// as whole words, its last word also with -s or -es, so "class" and
// "Scunthorpe" stay as written; a stem counts inside any word. Japanese and
// Chinese entries count anywhere. Words are compared in lower case, with
// look-alike digits and symbols read as letters (5h1t, @ss) and stretched
// letters read once (fuuuck). Text that is not well-formed UTF-8 is returned
// unchanged.
std::string FilterChat(const std::string& text);
} }
