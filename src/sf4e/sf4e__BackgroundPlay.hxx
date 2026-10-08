#pragma once

#include "../Dimps/Dimps__Pad.hxx"

// Background play (Player > Play in the background): the game's sound and the
// pads keep working while another window, such as OBS, is in front. The
// keyboard still needs the game window, and so do Ember's own menus.
// With or without the setting, a replay being exported as a video keeps
// playing and keeps its sound while the window is behind or minimized.
namespace sf4e {
	namespace BackgroundPlay {
		// Queues the hooks in the open Detours transaction.
		void Install();
		// Once that transaction committed: makes the frame's sound check and
		// both pad gates Ember's, or none of them, and only then lets the hooks act.
		void Activate();
		// Game thread, before the native pad update: records the setting and
		// does what the update's focus gate did, minus the pads when it is on.
		void BeforePadUpdate(Dimps::Pad::System* system, bool enabled);
		// Game thread: a replay is being exported as a video, or no longer.
		// The hooks, which the window procedure and the frame run, read only this.
		void HoldForExport(bool on);
	}
}
