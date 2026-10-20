#pragma once

// How far the game moves a battle each frame, from its step setting (see
// Dimps::App::battleStepSetting): below 0 is one frame, 0 is the measured
// length of the last frame, above 0 that many frames. config.ini
// FrameRate=SMOOTH, or no FrameRate line, leaves the setting at 0. Rollback
// runs frames back to back and the two PCs measure different lengths, so a
// match desyncs in its first second unless both step one frame at a time.
// The game's own online battles force that; Ember's matches must do the same.
//
// Pure component: no game or platform dependencies, unit tested.

namespace sf4e {
namespace battlestep {

// The setting that steps one frame at a time.
static const float OneFrame = -1.0f;

// The player's setting held across one match: Begin when the match's session
// starts, End when its battle closes.
class MatchStep {
public:
	// Remembers the setting the player had. True when it must be set to
	// OneFrame for the match. A second Begin before End keeps the first value.
	bool Begin(float current) {
		if (active_) return false;
		active_ = true;
		saved_ = current;
		return current != OneFrame;
	}
	// True when the player's setting (Saved) must be put back.
	bool End() {
		const bool restore = active_ && saved_ != OneFrame;
		active_ = false;
		return restore;
	}
	float Saved() const { return saved_; }

private:
	bool active_ = false;
	float saved_ = OneFrame;
};

} // namespace battlestep
} // namespace sf4e
