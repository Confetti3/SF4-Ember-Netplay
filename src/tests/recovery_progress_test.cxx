// Pure unit tests for which room recovery changes are logged.

#include "../session/RecoveryProgress.hxx"

#include "test_support.hxx"

using sf4e::session::RecoveryProgress;
using Change = RecoveryProgress::Change;

static const char* Refused = "replicated checkpoint import of revision 12 rejected: match_authority 0";
static const char* Rebinding = "replicated checkpoint rebind pending";

// Ordinary catch-up waits for a rebind without any failure: nothing to say.
static void TestCatchUpIsQuiet() {
	RecoveryProgress progress;
	CHECK(progress.Observe(false, Rebinding, 11) == Change::None);
	CHECK(progress.Observe(false, "", 12) == Change::None);
}

// A refusal is said once while it lasts, and a different one is said again.
static void TestFailureIsSaidOnce() {
	RecoveryProgress progress;
	CHECK(progress.Observe(true, Refused, 11) == Change::Failed);
	CHECK(progress.Observe(true, Refused, 11) == Change::None);
	CHECK(progress.Observe(true, "replicated checkpoint import of revision 12 rejected: members", 11) == Change::Failed);
}

// Once the import is accepted but still waits for its rebind or activation,
// that is its own state: recovery has not resumed until a commit applies.
static void TestResumesOnlyWhenACommitApplies() {
	RecoveryProgress progress;
	CHECK(progress.Observe(true, Refused, 11) == Change::Failed);
	CHECK(progress.Observe(false, Rebinding, 11) == Change::Waiting);
	CHECK(progress.Observe(false, Rebinding, 11) == Change::None);
	CHECK(progress.Observe(false, "committed checkpoint activation pending", 11) == Change::Waiting);
	CHECK(progress.Observe(false, "", 12) == Change::Resumed);
	CHECK(progress.Observe(false, "", 13) == Change::None);
	// A later failure is new, and measured from where it began.
	CHECK(progress.Observe(true, Refused, 13) == Change::Failed);
	CHECK(progress.Observe(false, Rebinding, 13) == Change::Waiting);
	CHECK(progress.Observe(false, "", 14) == Change::Resumed);
}

int main() {
	TestCatchUpIsQuiet();
	TestFailureIsSaidOnce();
	TestResumesOnlyWhenACommitApplies();
	return 0;
}
