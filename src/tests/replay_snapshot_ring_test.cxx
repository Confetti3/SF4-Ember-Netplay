#include "../common/MementoGuards.hxx"
#include "../common/ReplaySnapshotRing.hxx"
#include "test_support.hxx"

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
	mg::Stake stake = mg::Stake::Optional;
};
using Table = mg::PayloadOwners<Owner>;

// What the playback's Snap is to the ring: claims in the ownership table, usable until its claim is taken away.
struct Snap : rs::Entry {
	Owner owner;
	size_t Claims() const { return owner.guardClaims; }
	bool Usable() const { return !owner.keyFailure; }
};

struct Fixture {
	Table table;
	rs::Ring<Snap> ring;
	std::vector<int> payloads = std::vector<int>(1 << 20);
	size_t next = 0;
	size_t released = 0;

	void Release(Snap& snap) { table.Erase(&snap.owner); ++released; }
	auto Releaser() { return [this](Snap& snap) { Release(snap); }; }
	// False when the table refused a claim; the snapshot is then released whole, as Save does.
	bool Take(int round, int frame, size_t claims) {
		if (ring.NeedsThinning()) ring.Thin(Releaser());
		auto snap = std::make_unique<Snap>();
		snap->round = round; snap->frame = frame;
		for (size_t i = 0; i < claims; ++i) { Key saved; saved.mementos = &payloads[next++]; snap->owner.keys.push_back({nullptr, saved}); }
		bool full = false;
		for (size_t i = 0; i < snap->owner.keys.size(); ++i) if (table.Insert(&snap->owner, i) == mg::Claimed::Full) full = true;
		if (full) { table.Erase(&snap->owner); return false; }
		ring.Add(std::move(snap));
		return true;
	}
	// The playback's take: one when due, thinning first when the ring is full.
	bool Play(int round, int frame, size_t claims) { return !ring.Due(round, frame) || Take(round, frame, claims); }
	size_t Held() const { size_t held = 0; for (const auto& snap : ring.All()) held += snap->Claims(); return held; }
};

// The 24th snapshot ran the table out in the field (86 to 89 claims each); the count rule alone takes 23 of 88
// or 22 of 90 and refuses the next.
static void TestCountAloneFills() {
	const std::pair<size_t, size_t> cases[] = {{88, 23}, {90, 22}};
	for (const auto& expect : cases) {
		const size_t claims = expect.first, taken = expect.second;
		Table table; std::vector<std::unique_ptr<Owner>> owners; std::vector<int> payloads(1 << 16); size_t next = 0, count = 0;
		for (;;) {
			auto owner = std::make_unique<Owner>();
			for (size_t i = 0; i < claims; ++i) { Key saved; saved.mementos = &payloads[next++]; owner->keys.push_back({nullptr, saved}); }
			bool full = false;
			for (size_t i = 0; i < owner->keys.size(); ++i) if (table.Insert(owner.get(), i) == mg::Claimed::Full) full = true;
			if (full) break;
			owners.push_back(std::move(owner)); ++count;
		}
		CHECK(count == taken);
	}
}

static void TestClaimsKeepTheRingInTheTable() {
	for (size_t claims : {20, 60, 90, 130, 400}) {
		Fixture fixture;
		for (int frame = 0; frame < 120 * 600; frame += 120) CHECK_MSG(fixture.Play(0, frame, claims), "claims %zu, frame %d", claims, frame);
		CHECK(fixture.Held() <= rs::kClaimBudget && fixture.table.Size() == fixture.Held() && !fixture.ring.Empty());
		CHECK(fixture.ring.Earliest(0) && fixture.ring.Earliest(0)->frame == 0);
	}
	// A snapshot that grows with the fight is still taken.
	Fixture fixture;
	for (int n = 0; n < 300; ++n) CHECK(fixture.Play(0, n * 120, 40 + n % 120));
}

static void TestThinningRules() {
	rs::Ring<Snap> ring;
	CHECK(!ring.NeedsThinning());
	auto add = [&ring](int round, int frame, size_t claims) {
		auto snap = std::make_unique<Snap>(); snap->round = round; snap->frame = frame; snap->owner.guardClaims = claims; ring.Add(std::move(snap));
	};
	add(0, 0, 2000);
	CHECK(!ring.NeedsThinning());
	add(0, 120, 2000);
	CHECK(ring.NeedsThinning());
	// The count alone thins too, whatever the claims.
	rs::Ring<Snap> many;
	for (size_t n = 0; n < rs::kMostSnaps; ++n) {
		CHECK(!many.NeedsThinning());
		many.Add(std::make_unique<Snap>());
	}
	CHECK(many.NeedsThinning());
}

// A new round frees the old rounds' snapshots and starts the spacing over; thinning keeps each round's first.
static void TestRounds() {
	Fixture fixture;
	for (int frame = 0; frame < 120 * 20; frame += 120) CHECK(fixture.Play(0, frame, 90));
	CHECK(fixture.ring.Every() > rs::kFirstEvery);
	const size_t before = fixture.released;
	fixture.ring.DropOtherRounds(1, fixture.Releaser());
	CHECK(fixture.ring.Empty() && fixture.table.Size() == 0 && fixture.released > before && fixture.ring.Every() == rs::kFirstEvery);
	for (int frame = 30; frame < 30 + 120 * 100; frame += 120) CHECK(fixture.Play(1, frame, 90));
	CHECK(fixture.ring.Earliest(1) && fixture.ring.Earliest(1)->frame == 30);
	// The same round again frees nothing.
	const size_t size = fixture.ring.Size();
	fixture.ring.DropOtherRounds(1, fixture.Releaser());
	CHECK(fixture.ring.Size() == size);
	// Thinning across two rounds keeps the first of each.
	Fixture two;
	for (int frame = 0; frame < 600; frame += 120) CHECK(two.Take(0, frame, 10));
	for (int frame = 5; frame < 600; frame += 120) CHECK(two.Take(1, frame, 10));
	two.ring.Thin(two.Releaser());
	CHECK(two.ring.Earliest(0)->frame == 0 && two.ring.Earliest(1)->frame == 5);
	two.ring.Clear(two.Releaser());
	CHECK(two.ring.Empty() && two.table.Size() == 0);
}

// A snapshot whose claim was taken away is skipped, then dropped, and the jump falls back to the earlier one.
static void TestDamagedSnapshot() {
	Fixture fixture;
	for (int frame = 0; frame < 600; frame += 120) CHECK(fixture.Take(0, frame, 90));
	const Snap* fourth = fixture.ring.Best(0, 360);
	CHECK(fourth && fourth->frame == 360);
	const_cast<Snap*>(fourth)->owner.keyFailure = true;
	const Snap* fallback = fixture.ring.Best(0, 400);
	CHECK(fallback && fallback->frame == 240);
	CHECK(fixture.ring.Earliest(0)->frame == 0);
	const size_t held = fixture.table.Size();
	fixture.ring.DropUnusable(fixture.Releaser());
	CHECK(fixture.ring.Size() == 4 && fixture.table.Size() == held - 90 && fixture.ring.Best(0, 400)->frame == 240);
	// Damage to every one of them leaves nowhere to go back to.
	for (const auto& snap : fixture.ring.All()) snap->owner.keyFailure = true;
	CHECK(!fixture.ring.Best(0, 600) && !fixture.ring.Earliest(0));
}

// A failure is the match's unless it is about optional states alone.
static void TestResponses() {
	using R = mg::Response;
	Owner snapshot, otherSnapshot, slot;
	slot.stake = mg::Stake::Match;
	const auto respond = [](bool restoring, unsigned depth, bool optional, bool closing, bool draining) { return mg::RespondToFailure(restoring, depth, optional, closing, draining); };
	CHECK(respond(false, 0, false, false, false) == R::Abort);
	CHECK(respond(false, 0, true, false, false) == R::Ignore);
	CHECK(respond(true, 0, true, false, false) == R::FailRestore);
	CHECK(respond(false, 1, true, false, false) == R::FailRestore);
	CHECK(respond(false, 0, false, true, false) == R::Ignore);
	CHECK(respond(false, 0, false, false, true) == R::Ignore);
	// A snapshot's own failure is ignored; so is one between two snapshots.
	CHECK(mg::OnlyOptional(&snapshot));
	CHECK(mg::OnlyOptional(&snapshot, &otherSnapshot));
	// One about another state (a netplay slot whose payload a snapshot duplicated, or the live battle) is not.
	CHECK(!mg::OnlyOptional(&snapshot, &slot) && !mg::OnlyOptional(&slot) && !mg::OnlyOptional<Owner>(nullptr));
	CHECK(respond(false, 0, mg::OnlyOptional(&snapshot, &slot), false, false) == R::Abort);
	CHECK(respond(false, 0, mg::OnlyOptional<Owner>(nullptr), false, false) == R::Abort);
}

int main() {
	TestCountAloneFills();
	TestClaimsKeepTheRingInTheTable();
	TestThinningRules();
	TestRounds();
	TestDamagedSnapshot();
	TestResponses();
	return 0;
}
