#pragma once

// Endless lobby music (Settings > Endless lobby music, on by default): the main
// menu plays a stage theme from the game's lobby pool instead of looping its
// menu track, another one after every match. Nothing on disk changes.
namespace sf4e {
	namespace MenuMusic {
		// Game thread, at start-up and when the setting is saved: a theme, or
		// the menu's own track back. The menu uses it the next time it starts
		// its music.
		void Use(bool endless);
		// Game thread, as a battle closes: with the setting on, another theme.
		void Pick();
	}
}
