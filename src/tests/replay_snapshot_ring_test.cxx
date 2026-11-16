#include "../common/MementoGuards.hxx"
#include "../common/ReplaySnapshotRing.hxx"
#include "test_support.hxx"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace mg = sf4e::memento;
namespace rs = sf4e::replaysnaps;

struct Key { void* mementos = nullptr; };
struct Owner {
	std::vector<std::pair<Key*, Key>> keys;
	bool keyFailure = false;
	size_t guardClaims = 0;
};

// A replay's snapshots against the ownership table, as the playback takes and
// thins them: each claims `perSnap` payloads of its own.
struct Ring {
	mg::PayloadOwners<Owner> owners;
	std::vector<std::unique_ptr<Owner>> snaps;
	std::vector<int> payloads = std::vector<int>(1 << 20);
	size_t next = 0;
	bool byClaims;
	explicit Ring(bool claims) : byClaims(claims) {}

	// False when the table refused a claim; the snapshot is then released whole, as Save does.
	bool Take(size_t perSnap) {
		size_t held = 0, largest = 0;
		for (const auto& s : snaps) { held += s->guardClaims; largest = (std::max)(largest, s->guardClaims); }
		const bool thin = byClaims ? rs::NeedsThinning(snaps.size(), held, largest) : snaps.size() >= rs::kMostSnaps;
		if (thin) Thin();
		auto snap = std::make_unique<Owner>();
		for (size_t i = 0; i < perSnap; ++i) { Key saved; saved.mementos = &payloads[next++]; snap->keys.push_back({nullptr, saved}); }
		bool full = false;
		for (size_t i = 0; i < snap->keys.size(); ++i) if (owners.Insert(snap.get(), i) == mg::Claimed::Full) full = true;
		if (full) { owners.Erase(snap.get()); return false; }
		snaps.push_back(std::move(snap));
		return true;
	}
	void Thin() {
		std::vector<std::unique_ptr<Owner>> kept;
		for (size_t i = 0; i < snaps.size(); ++i) {
			if (i % 2 == 0) kept.push_back(std::move(snaps[i]));
			else owners.Erase(snaps[i].get());
		}
		snaps = std::move(kept);
	}
};

// A fight's snapshots claim about 90 payloads each; the count rule alone ran
// the table out on the 24th, which is a replay's 52nd second.
static void TestCountAloneFills() {
	Ring ring(false);
	size_t taken = 0;
	while (ring.Take(90)) ++taken;
	CHECK(taken == 22);
}

static void TestClaimsKeepTheRingInTheTable() {
	for (size_t perSnap : {20, 60, 90, 130, 400}) {
		Ring ring(true);
		for (int snapshot = 0; snapshot < 600; ++snapshot) CHECK_MSG(ring.Take(perSnap), "per snapshot %zu, number %d", perSnap, snapshot);
		size_t held = 0;
		for (const auto& s : ring.snaps) held += s->guardClaims;
		CHECK(held <= rs::kClaimBudget && ring.owners.Size() == held && !ring.snaps.empty());
	}
	// A snapshot that grows with the fight (more objects later) is still taken.
	Ring ring(true);
	for (size_t snapshot = 0; snapshot < 300; ++snapshot) CHECK(ring.Take(40 + snapshot % 120));
}

static void TestNeverThinsToNothing() {
	CHECK(!rs::NeedsThinning(0, 0, 0) && !rs::NeedsThinning(1, 2000, 2000));
	CHECK(rs::NeedsThinning(rs::kMostSnaps, 100, 4) && rs::NeedsThinning(2, 1500, 90) && !rs::NeedsThinning(10, 900, 90));
}

// An optional save (a replay's snapshot) fails alone; the other failures keep their answers.
static void TestResponses() {
	using R = mg::Response;
	CHECK(mg::RespondToFailure(false, 0, 0, false, false) == R::Abort);
	CHECK(mg::RespondToFailure(false, 0, 1, false, false) == R::Ignore);
	CHECK(mg::RespondToFailure(true, 0, 1, false, false) == R::FailRestore);
	CHECK(mg::RespondToFailure(false, 1, 1, false, false) == R::FailRestore);
	CHECK(mg::RespondToFailure(false, 0, 0, true, false) == R::Ignore);
	CHECK(mg::RespondToFailure(false, 0, 0, false, true) == R::Ignore);
}

int main() {
	TestCountAloneFills();
	TestClaimsKeepTheRingInTheTable();
	TestNeverThinsToNothing();
	TestResponses();
	return 0;
}
