#pragma once

#include "../training/MoveNotation.hxx"
#include "../training/RecordingTypes.hxx"

// The tournament host's fighter (sf4e__SelfTest__Tournament.cxx), apart from
// sf4e__SelfTest.hxx so the production files that include that one do not
// pick up the training headers.
namespace sf4e { namespace selftest {

// What the host's fighter plays on its `frame`th read: forward held
// throughout, heavy punch pressed for two frames out of every twenty. A
// fighter who stands still is hit until the game ends, so it ends in the
// host's win rather than a draw on time, which a tournament plays again.
constexpr unsigned kAttackPeriod = 20, kAttackPress = 2;
inline unsigned AttackInput(unsigned frame, bool facingRight) {
	const unsigned forward = facingRight ? training::RightBit : training::LeftBit;
	return forward | (frame % kAttackPeriod < kAttackPress ? combo::HP : 0u);
}

} }
