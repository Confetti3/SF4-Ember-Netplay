#pragma once

// Pairing step of fSoundPlayerManager::SyncState. Pure and game-free so it
// can be unit tested.
//
// Every live stub adapter is paired with a distinct live real adapter that
// holds the same request. Pairing finishes before SyncState stops or starts
// anything, so a real sound started for one stub can never be claimed by a
// later identical stub (ledger A-006), and a surplus real duplicate is left
// unpaired and stopped instead of playing on unmanaged.

#include <vector>

namespace sf4e {
namespace sound {

struct SoundPairing {
	std::vector<int> realForStub;  // Real adapter per stub, or -1 (idle, or needs a new sound).
	std::vector<bool> realPaired;  // Real adapters claimed by a stub; live unpaired ones get stopped.
};

// Fills `pairing`, reusing its storage: SyncState runs around every accepted
// frame, so it must not allocate once warmed up.
template <class StubLive, class RealLive, class Same>
void PairLiveSounds(SoundPairing& pairing, int stubCount, int realCount, StubLive stubLive, RealLive realLive, Same same) {
	pairing.realForStub.assign(stubCount < 0 ? 0 : stubCount, -1);
	pairing.realPaired.assign(realCount < 0 ? 0 : realCount, false);
	for (int stub = 0; stub < stubCount; stub++) {
		if (!stubLive(stub)) continue;
		for (int real = 0; real < realCount; real++) {
			if (pairing.realPaired[real] || !realLive(real) || !same(stub, real)) continue;
			pairing.realPaired[real] = true;
			pairing.realForStub[stub] = real;
			break;
		}
	}
}

} // namespace sound
} // namespace sf4e
