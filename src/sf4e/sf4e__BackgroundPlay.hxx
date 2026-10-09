#pragma once

namespace Dimps { namespace Pad { struct System; } }

// Background play (Player > Play in the background): the game's sound and the
// pads keep working while another window, such as OBS, is in front. The
// keyboard still needs the game window, and so do Ember's own menus.
// With or without the setting, a replay being exported as a video keeps
// playing and keeps its sound while the window is behind or minimized, and
// a room's match this PC only watches keeps playing there too.
namespace sf4e {
	namespace BackgroundPlay {
		// What the game keeps doing while its window is not in front. The caller
		// knows why; this module only carries it out.
		struct Policy {
			// The player's setting: the pads are polled behind another window and
			// only the keyboard is cleared, instead of every input.
			bool backgroundInput = false;
			// The sound stays up behind another window.
			bool keepSound = false;
			// The game goes on running behind another window or minimized.
			bool keepRunning = false;
			// The sound stays up while minimized too.
			bool keepSoundMinimized = false;
		};
		// The policy for the setting, a replay being exported as a video, and a
		// room's match this PC only watches. An export is heard wherever the
		// window is; a watched match runs on but stays muted behind as before.
		inline Policy PolicyFor(bool setting, bool exporting, bool watching) {
			Policy policy;
			policy.backgroundInput = setting;
			policy.keepSound = setting || exporting;
			policy.keepRunning = exporting || watching;
			policy.keepSoundMinimized = exporting;
			return policy;
		}
		// Queues the hooks in the open Detours transaction.
		void Install();
		// Once that transaction committed: makes the frame's sound check and
		// both pad gates Ember's, or none of them, and only then lets the hooks act.
		void Activate();
		// Game thread, before the native pad update: records the policy and
		// does what the update's focus gate did, minus the pads when the
		// player's setting is on.
		void BeforePadUpdate(Dimps::Pad::System* system, const Policy& policy);
	}
}
