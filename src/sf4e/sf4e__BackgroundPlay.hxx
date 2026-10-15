#pragma once

namespace Dimps { namespace Pad { struct System; } }
struct HWND__;

// Background play (Player > Play in the background): the game's sound and the
// pads keep working while another window, such as OBS, is in front. The
// keyboard still needs the game window.
// With or without the setting, a replay being exported as a video keeps
// playing and keeps its sound while the window is behind or minimized, and
// a room's match this PC only watches keeps playing there too. Ember's menu
// keeps the pads behind another window too, while it is shown and where
// Start opens it (the main menu, a Practice match this PC fights), and so does go now while a call back from
// Training offers it; without the setting the game is given none of them there.
namespace sf4e {
	namespace BackgroundPlay {
		// What the game keeps doing while its window is not in front. The caller
		// knows why; this module only carries it out.
		struct Policy {
			// The pads are polled behind another window and only the keyboard
			// is cleared, instead of every input.
			bool backgroundInput = false;
			// They are polled there for Ember's menu alone: the game is given
			// no input behind another window, as without the setting.
			bool menuOnly = false;
			// The sound stays up behind another window.
			bool keepSound = false;
			// The game goes on running behind another window or minimized.
			bool keepRunning = false;
			// The sound stays up while minimized too.
			bool keepSoundMinimized = false;
		};
		// The policy for the setting, a replay being exported as a video, a
		// room's match this PC only watches, and Ember taking the pads: its menu
		// (ui::MenuPads::Takes) or go now on a call back from Training. An
		// export is heard wherever the window is; a
		// watched match runs on but stays muted behind as before. The menu
		// changes only what is polled, never the sound or the running.
		inline Policy PolicyFor(bool setting, bool exporting, bool watching, bool menu = false) {
			Policy policy;
			policy.backgroundInput = setting || menu;
			policy.menuOnly = menu && !setting;
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
		// does what the update's focus gate did, minus the pads when they are
		// polled behind another window.
		void BeforePadUpdate(Dimps::Pad::System* system, const Policy& policy);
		// Whether the game is given none of an update's input: the pads were
		// polled behind another window for Ember alone. Asked where the update
		// publishes its input, with the window as it stands there, so a window
		// that went behind after the gate before the update still keeps from
		// the game what was read for Ember.
		inline bool HidesInput(const Policy& policy, bool windowInFront) { return policy.menuOnly && !windowInFront; }
		// Game thread, after the native pad update: HidesInput for the window now.
		bool HidesFromGame(const Policy& policy);
		// The game's window, as the last pad update found it; null before.
		HWND__* GameWindow();
	}
}
