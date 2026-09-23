#include "../common/SoundReconcile.hxx"

#include <vector>

#include "test_support.hxx"

using sf4e::sound::PairLiveSounds;

// Requests are plain ints here; 0 means the adapter is idle.
static sf4e::sound::SoundPairing Pair(const std::vector<int>& stubs, const std::vector<int>& reals) {
	sf4e::sound::SoundPairing pairing;
	PairLiveSounds(pairing, static_cast<int>(stubs.size()), static_cast<int>(reals.size()),
		[&](int stub) { return stubs[stub] != 0; },
		[&](int real) { return reals[real] != 0; },
		[&](int stub, int real) { return stubs[stub] == reals[real]; });
	return pairing;
}

int main() {
	{
		// A-006 as reported: two identical stub sounds start with no real
		// copy. The old reconciliation started a sound for the first stub,
		// then matched the second stub to that same new adapter. Pairing now
		// happens before anything starts, so each stub starts its own sound.
		const auto pairing = Pair({7, 7}, {0, 0, 0});
		CHECK((pairing.realForStub == std::vector<int>{-1, -1}));
	}
	{
		// A-006: two identical live stub sounds and one real copy. The first
		// stub takes the real copy; the second must start its own sound
		// rather than share the adapter.
		const auto pairing = Pair({7, 7, 0}, {0, 7, 0});
		CHECK((pairing.realForStub == std::vector<int>{1, -1, -1}));
		CHECK((pairing.realPaired == std::vector<bool>{false, true, false}));
	}
	{
		// Two identical copies on each side pair one to one.
		const auto pairing = Pair({7, 7}, {7, 7});
		CHECK((pairing.realForStub == std::vector<int>{0, 1}));
	}
	{
		// A surplus real duplicate stays unpaired, so SyncState stops it.
		const auto pairing = Pair({7, 0, 0}, {7, 7, 3});
		CHECK((pairing.realForStub == std::vector<int>{0, -1, -1}));
		CHECK((pairing.realPaired == std::vector<bool>{true, false, false}));
	}
	{
		// Real adapters beyond the stub count are still considered.
		const auto pairing = Pair({5}, {0, 0, 0, 5});
		CHECK((pairing.realForStub == std::vector<int>{3}));
	}
	{
		// Reused storage: a second, smaller pairing does not reallocate and
		// leaves no stale entries from the first.
		sf4e::sound::SoundPairing pairing;
		const auto live = [](int) { return true; };
		const auto same = [](int, int) { return true; };
		PairLiveSounds(pairing, 4, 4, live, live, same);
		const int* storage = pairing.realForStub.data();
		PairLiveSounds(pairing, 2, 3, live, live, same);
		CHECK(pairing.realForStub.data() == storage);
		CHECK((pairing.realForStub == std::vector<int>{0, 1}));
		CHECK((pairing.realPaired == std::vector<bool>{true, true, false}));
	}
	return 0;
}
