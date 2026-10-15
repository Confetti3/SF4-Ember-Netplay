// The per-frame trace a desync report logs (common/FrameTrace.hxx), the frame
// identity and order its rows are sampled in (common/GgpoTimeline.hxx), and the
// order mismatches reach it in (common/ConfirmedCheckpoint.hxx).

#include <stdio.h>

#include <map>
#include <vector>

#include "../common/ConfirmedCheckpoint.hxx"
#include "../common/FrameTrace.hxx"
#include "../common/GgpoTimeline.hxx"

using sf4e::statehash::FrameTrace;

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			g_failures++;                                                    \
		}                                                                    \
	} while (0)

struct Row {
	int value = 0;
};

using Trace = FrameTrace<Row, 8>;

// A rollback re-simulates frames already traced: each pass replaces the row
// and is counted, so a confirmed frame keeps the corrected timeline's values.
static void ResimulationReplacesTheRowAndCountsThePass() {
	Trace trace;
	trace.Begin(5).row.value = 1;
	CHECK(trace.Find(5)->passes == 1);
	Trace::Entry& again = trace.Begin(5);
	CHECK(again.row.value == 0);
	again.row.value = 2;
	CHECK(trace.Find(5)->passes == 2);
	CHECK(trace.Find(5)->row.value == 2);
}

// A slot taken by a later frame no longer answers for the earlier one, and
// that later frame starts with a single pass.
static void ALaterFrameTakesTheSlotOver() {
	Trace trace;
	trace.Begin(3).row.value = 3;
	trace.Begin(3);
	trace.Begin(11).row.value = 11;
	CHECK(trace.Find(3) == nullptr);
	CHECK(trace.Find(11)->passes == 1);
	CHECK(trace.Find(11)->row.value == 11);
}

static void UntracedFramesAreNotFound() {
	Trace trace;
	CHECK(trace.Find(0) == nullptr);
	CHECK(trace.Find(-1) == nullptr);
	trace.Begin(4);
	CHECK(trace.Find(12) == nullptr);
	trace.Clear();
	CHECK(trace.Find(4) == nullptr);
	CHECK(trace.Begin(4).passes == 1);
}

// GGPO as the battle drives it, through the GgpoTimeline the battle uses
// (PlayGgpoFrame, ggpo_advance_frame_callback, the save and load callbacks).
// The engine's state is the sum of the inputs it played. An advance saves the
// frame it reached and polls, as GGPO's IncrementFrame does. A poll that
// learns an input GGPO had predicted loads the frame before it and
// re-simulates back to where it was, as Sync::AdjustSimulation does; those
// advances do not poll.
struct FakeGgpo {
	sf4e::statehash::GgpoTimeline timeline;
	int frame = 0;               // GGPO's own frame count
	int state = 0;
	std::map<int, int> saves;    // state frame -> engine state
	std::map<int, int> known;    // input frame -> real input
	std::map<int, int> arriving; // inputs the next poll receives
	int predicted = 1;           // the remote input GGPO repeats until the real one arrives
	bool rollingBack = false;
	FrameTrace<Row, 16> trace;
	std::map<int, int> checkpoints; // state frame -> state, from the same sample

	// GGPO saves frame 0 before the first input.
	FakeGgpo() { Save(); }

	bool PlayFrame() {
		const auto it = known.find(frame);
		const int input = it != known.end() ? it->second : predicted;
		return sf4e::statehash::GgpoTimeline::Simulate(
			[&] { state += input; },
			[&] {
				const int reached = timeline.Next();
				trace.Begin(reached).row.value = input * 1000 + state;
				checkpoints[reached] = state;
			},
			[&] { return Advance(); });
	}

private:
	void Save() {
		saves[frame] = state;
		timeline.Saved(frame);
	}
	bool Advance() {
		++frame;
		Save();
		if (!rollingBack && !arriving.empty()) Poll();
		return true;
	}
	void Poll() {
		const int from = arriving.begin()->first, to = frame;
		known.insert(arriving.begin(), arriving.end());
		arriving.clear();
		rollingBack = true;
		frame = from;
		CHECK(timeline.Load(from, [&] { state = saves[from]; return true; }));
		while (frame < to) PlayFrame();
		rollingBack = false;
	}
};

// The advance that saves a frame may poll and roll back over it. The frame
// it re-simulates with the real input replaces the predicted sample, so the
// frame keeps the corrected input with the state it made, two passes, and a
// checkpoint from that same sample.
static void ARollbackInsideTheAdvanceKeepsTheCorrectedSample() {
	FakeGgpo ggpo;
	for (int i = 0; i < 4; i++) ggpo.PlayFrame();
	// Input 4 is played as predicted (1); its real value (2) arrives with
	// the poll of the very advance that saves state 5.
	ggpo.arriving[4] = 2;
	ggpo.PlayFrame();
	CHECK(ggpo.timeline.Held() == 5);
	const auto* entry = ggpo.trace.Find(5);
	CHECK(entry != nullptr);
	if (!entry) return;
	CHECK(entry->passes == 2);
	CHECK(entry->row.value == 2 * 1000 + 6);
	CHECK(ggpo.checkpoints[5] == 6);
	CHECK(ggpo.trace.Find(4)->passes == 1);
	CHECK(ggpo.trace.Find(4)->row.value == 1 * 1000 + 4);
	CHECK(ggpo.trace.Find(6) == nullptr);
}
struct Checkpoint {
	int frame;
	int ggpoStateFrame;
};

// Frames 1890 and 1920 sit in the ring's last and first slots. Waiting
// together, the earlier is compared first, so the one trace a battle logs
// ends at 1890 (frames 1860 to 1890) on both PCs; later mismatches log none
// until the battle starts again.
static void TheEarliestPendingMismatchTakesTheTrace() {
	using namespace sf4e::statehash;
	CHECK(CheckpointRingIndex(1890) == CheckpointRingSize - 1);
	CHECK(CheckpointRingIndex(1920) == 0);
	const Checkpoint local[] = { { 1920, 1920 }, { 1890, 1890 } }; // ring order
	const auto find = [&](int frame) -> const Checkpoint* {
		for (const Checkpoint& checkpoint : local) if (checkpoint.frame == frame) return &checkpoint;
		return nullptr;
	};
	Trace trace;
	std::vector<int> reported, dumped;
	const auto report = [&](const Checkpoint& checkpoint, int) {
		reported.push_back(checkpoint.frame);
		if (trace.TakeDump()) dumped.push_back(checkpoint.frame);
	};
	std::map<int, int> pending = { { 1920, 0 }, { 1890, 0 } };
	ReconcileRemoteHashes(pending, 1920, find, report);
	CHECK(reported == std::vector<int>({ 1890, 1920 }));
	CHECK(dumped == std::vector<int>({ 1890 }));
	CHECK(pending.empty());
	CHECK(!trace.TakeDump());
	// A new battle logs its own first mismatch.
	trace.Clear();
	CHECK(trace.TakeDump());

	// A checkpoint not yet confirmed here keeps its remote hash waiting.
	pending = { { 1920, 0 }, { 1890, 0 } };
	reported.clear();
	ReconcileRemoteHashes(pending, 1900, find, report);
	CHECK(reported == std::vector<int>({ 1890 }));
	CHECK(pending.size() == 1 && pending.count(1920) == 1);
}

int main() {
	ResimulationReplacesTheRowAndCountsThePass();
	ALaterFrameTakesTheSlotOver();
	UntracedFramesAreNotFound();
	ARollbackInsideTheAdvanceKeepsTheCorrectedSample();
	TheEarliestPendingMismatchTakesTheTrace();
	if (g_failures) {
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	printf("FrameTrace: all passed\n");
	return 0;
}
