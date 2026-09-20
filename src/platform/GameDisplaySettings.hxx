#pragma once
#include "../common/GameDisplayConfig.hxx"

namespace sf4e { namespace platform {

// The game reads Documents\CAPCOM\SUPERSTREETFIGHTERIV\config.ini at startup,
// so one read per launch sees what the game is running with. Missing file,
// missing keys and unreadable bytes all parse to empty, which is recommended.
const gameconfig::DisplaySettings& GameDisplaySettings();

} } // namespace sf4e::platform
