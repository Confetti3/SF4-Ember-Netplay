// Pure unit tests for the helper error scope table (F-017).

#include "../session/HelperErrorScope.hxx"
#include "../session/RoomFailure.hxx"

#include <string>

#include "test_support.hxx"

using sf4e::session::ClassifyHelperError;
using sf4e::session::HelperErrorScope;
using sf4e::session::HelperErrorScopeName;

static HelperErrorScope Scope(const char* code, bool coordination = true, bool otherPeer = false) {
	return ClassifyHelperError(code, coordination, otherPeer).scope;
}

static bool Labelled(const char* code, bool coordination = true) {
	return ClassifyHelperError(code, coordination, false).codeIsProtocolLabel;
}

static void TestProbeAndGameplayStayLocal() {
	CHECK(Scope("probe_unavailable") == HelperErrorScope::Probe);
	CHECK(Scope("probe_unavailable", false) == HelperErrorScope::Probe);
	CHECK(Scope("gameplay_prepare_failed") == HelperErrorScope::Gameplay);
	CHECK(Scope("gameplay_prepare_failed", false) == HelperErrorScope::Gameplay);
}

static void TestCheckpointErrorsRetryUnderCoordination() {
	const char* codes[] = {
		"checkpoint_ack_rejected", "checkpoint_begin_rejected", "checkpoint_chunk_rejected",
		"checkpoint_not_committed", "checkpoint_receive_timeout", "checkpoint_send_timeout",
		"checkpoint_transfer_busy", "checkpoint_transfer_busy_incoming", "checkpoint_transfer_busy_outgoing",
		"checkpoint_transfer_busy_proposal", "checkpoint_transfer_missing", "invalid_checkpoint_ack",
		"invalid_checkpoint_chunk", "invalid_checkpoint_end", "stale_checkpoint_ack", "unexpected_checkpoint_ack",
	};
	for (const char* code : codes) {
		CHECK(Scope(code) == HelperErrorScope::Checkpoint);
		// Without committed coordination there is nothing to retry.
		CHECK(Scope(code, false) == HelperErrorScope::RoomFatal);
		CHECK(!Labelled(code, false));
	}
}

static void TestControlErrorsFollowThePeer() {
	CHECK(Scope("control_send_failed", true, true) == HelperErrorScope::ControlPeer);
	CHECK(Scope("control_send_failed", true, false) == HelperErrorScope::ControlLeader);
	CHECK(Scope("coordination_unavailable", true, true) == HelperErrorScope::ControlPeer);
	CHECK(Scope("coordination_unavailable", true, false) == HelperErrorScope::ControlLeader);
	// A legacy room has no committed leader to fall back on.
	CHECK(Scope("control_send_failed", false) == HelperErrorScope::RoomFatal);
	CHECK(Labelled("control_send_failed", false));
	CHECK(Scope("coordination_unavailable", false) == HelperErrorScope::RoomFatal);
	CHECK(!Labelled("coordination_unavailable", false));
}

static void TestMatchErrorsEndOnlyTheMatch() {
	CHECK(Scope("stale_match") == HelperErrorScope::Match);
	CHECK(Scope("stale_match", false) == HelperErrorScope::Match);
	CHECK(Scope("invalid_game_registration") == HelperErrorScope::Match);
	CHECK(Labelled("stale_match"));
}

static void TestRoomFatalCodesAndTheirLabels() {
	const char* shown[] = {
		"invalid_or_incompatible_invitation", "join_failed", "host_unavailable", "invalid_room_state", "leave_in_progress",
		"invalid_control_size",
	};
	for (const char* code : shown) {
		CHECK(Scope(code) == HelperErrorScope::RoomFatal);
		CHECK(Labelled(code));
	}
	const char* hidden[] = { "stale_epoch", "async_command_not_dispatched", "something_new", "" };
	for (const char* code : hidden) {
		CHECK(Scope(code) == HelperErrorScope::RoomFatal);
		CHECK(!Labelled(code));
	}
}

static void TestScopeNames() {
	CHECK(std::string(HelperErrorScopeName(HelperErrorScope::Probe)) == "probe");
	CHECK(std::string(HelperErrorScopeName(HelperErrorScope::Match)) == "match");
	CHECK(std::string(HelperErrorScopeName(HelperErrorScope::RoomFatal)) == "room");
	CHECK(std::string(HelperErrorScopeName(HelperErrorScope::ControlLeader)) == "control_leader");
}

static void TestFailureStageFromReason() {
	using sf4e::session::FailureStage;
	using sf4e::session::FailureStageFromHelper;
	using sf4e::session::FailureStageLabel;
	CHECK(FailureStageFromHelper("join_failed", "relay_unreachable") == FailureStage::RelayUnreachable);
	CHECK(FailureStageFromHelper("join_failed", "host_unreachable") == FailureStage::HostUnreachable);
	CHECK(FailureStageFromHelper("join_failed", "control_lost") == FailureStage::ControlLost);
	CHECK(FailureStageFromHelper("host_unavailable", "relay_unreachable") == FailureStage::RelayUnreachable);
	// An older helper sends no reason; an unknown reason or another code is not a stage.
	CHECK(FailureStageFromHelper("join_failed", "") == FailureStage::Unknown);
	CHECK(FailureStageFromHelper("join_failed", "https://use1-1.relay.n0.iroh.link./") == FailureStage::Unknown);
	CHECK(FailureStageFromHelper("control_send_failed", "relay_unreachable") == FailureStage::Unknown);
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "expired") == FailureStage::InviteExpired);
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "other_build") == FailureStage::InviteOtherBuild);
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "old_version") == FailureStage::InviteOtherBuild);
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "malformed") == FailureStage::InviteMalformed);
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "own_room") == FailureStage::InviteOwnRoom);
	CHECK(FailureStageFromHelper("join_failed", "own_room") == FailureStage::Unknown);
	CHECK(std::string(FailureStageLabel(FailureStage::InviteOwnRoom)) == "invite_own_room");
	// A short link the link service could not open, or has no room for.
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "short_unavailable") == FailureStage::ShortUnavailable);
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "short_unknown") == FailureStage::ShortUnknown);
	CHECK(FailureStageFromHelper("join_failed", "short_unknown") == FailureStage::Unknown);
	CHECK(std::string(FailureStageLabel(FailureStage::ShortUnavailable)) == "short_unavailable");
	CHECK(std::string(FailureStageLabel(FailureStage::ShortUnknown)) == "short_unknown");
	CHECK(FailureStageFromHelper("invalid_or_incompatible_invitation", "") == FailureStage::Unknown);
	CHECK(FailureStageFromHelper("join_failed", "expired") == FailureStage::Unknown);
	CHECK(std::string(FailureStageLabel(FailureStage::ControlLost)) == "control_lost");
	// The failures that carry a stage are still room-fatal protocol labels.
	CHECK(Scope("join_failed") == HelperErrorScope::RoomFatal && Labelled("join_failed"));
	CHECK(Scope("host_unavailable") == HelperErrorScope::RoomFatal && Labelled("host_unavailable"));
}

int main() {
	TestProbeAndGameplayStayLocal();
	TestCheckpointErrorsRetryUnderCoordination();
	TestControlErrorsFollowThePeer();
	TestMatchErrorsEndOnlyTheMatch();
	TestRoomFatalCodesAndTheirLabels();
	TestScopeNames();
	TestFailureStageFromReason();
	printf("helper_error_scope_test: all tests passed\n");
	return 0;
}
