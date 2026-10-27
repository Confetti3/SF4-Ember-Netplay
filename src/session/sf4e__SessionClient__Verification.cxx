// SessionClient: desync verification. Legacy v1 snapshots and v2 hash
// checkpoints are published by players and compared by every client;
// read-only on the game thread.

#include <stdlib.h>
#include <algorithm>
#include <string>
#include <utility>
#include <limits>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <ggponet.h>

#include "../Dimps/Dimps.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/ConfirmedCheckpoint.hxx"
#include "../common/sf4e__StateHash.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

#include "../sf4e/sf4e__Game__Battle__System.hxx"

#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
namespace session = sf4e::session;
using rSystem = Dimps::Game::Battle::System;
using fSystem = sf4e::Game::Battle::System;
using sf4e::SessionClient;

// Bound for buffered remote v2 hashes (matches the checkpoint ring span).
static const size_t MAX_PENDING_REMOTE_HASHES = 64;
// The opponent is at most a rollback window ahead, a few snapshots, so a
// small window is kept; compared snapshots are removed as they match.
static const size_t MAX_PENDING_REMOTE_SNAPSHOTS = 64;

// The legacy snapshot comparison is a whole-struct memcmp, so a mismatch used
// to be reported as "Desync detected!" and nothing else. That is not enough to
// find a determinism bug afterwards: name the exact frame and the exact fields
// that diverged, with both values, so a player's log identifies the subsystem.
// StateSnapshot is 4-byte-aligned throughout with no padding, so every byte the
// memcmp sees is covered by the fields below.
static void ReportSnapshotDivergence(
	const char* stage,
	const sf4e::SessionProtocol::StateSnapshot& local,
	const sf4e::SessionProtocol::StateSnapshot& remote
) {
	spdlog::error("Desync v1 ({}): mismatch @ local frame {} remote frame {}",
		stage, local.frameIdx, remote.frameIdx);
	if (local.frameIdx != remote.frameIdx) {
		spdlog::error("Desync v1 ({}): frameIdx differs; snapshots are not for the same frame", stage);
	}
	const auto fixed = [](const Dimps::Math::FixedPoint& value) {
		return static_cast<int>(value.integral) + static_cast<double>(value.fractional) / 65536.0;
	};
	for (int i = 0; i < 2; i++) {
		const auto& a = local.chara[i];
		const auto& b = remote.chara[i];
		if (a.status != b.status) spdlog::error("Desync v1 ({}): chara{} status {} != {}", stage, i, a.status, b.status);
		if (a.side != b.side) spdlog::error("Desync v1 ({}): chara{} side {} != {}", stage, i, a.side, b.side);
		for (int axis = 0; axis < 4; axis++) {
			if (memcmp(&a.rootPos[axis], &b.rootPos[axis], sizeof(float)) != 0) {
				spdlog::error("Desync v1 ({}): chara{} rootPos[{}] {:a} != {:a}",
					stage, i, axis, a.rootPos[axis], b.rootPos[axis]);
			}
		}
		const struct { const char* name; const Dimps::Math::FixedPoint& mine; const Dimps::Math::FixedPoint& theirs; } fields[] = {
			{"vit", a.vit, b.vit}, {"vitmax", a.vitmax, b.vitmax},
			{"revenge", a.revenge, b.revenge}, {"revengemax", a.revengemax, b.revengemax},
			{"recoverable", a.recoverable, b.recoverable}, {"recoverablemax", a.recoverablemax, b.recoverablemax},
			{"super", a.super, b.super}, {"supermax", a.supermax, b.supermax},
			{"sctimeamt", a.sctimeamt, b.sctimeamt}, {"sctimemax", a.sctimemax, b.sctimemax},
			{"uctime", a.uctime, b.uctime}, {"uctimemax", a.uctimemax, b.uctimemax},
			{"damage", a.damage, b.damage}, {"combodamage", a.combodamage, b.combodamage},
		};
		for (const auto& field : fields) {
			if (field.mine.integral != field.theirs.integral || field.mine.fractional != field.theirs.fractional) {
				spdlog::error("Desync v1 ({}): chara{} {} {:.4f} != {:.4f}",
					stage, i, field.name, fixed(field.mine), fixed(field.theirs));
			}
		}
	}
}

// Strict debug mode: terminate the match on an authoritative v2 mismatch.
// Default (unset) logs and reports only; v2 must not end release matches
// while it is being validated; the legacy snapshot system retains its
// existing termination behavior.
static bool StrictDesyncEnabled() {
	static int s_cached = -1;
	if (s_cached < 0) {
		const char* env = getenv("SF4E_STRICT_DESYNC");
		s_cached = (env && env[0] == '1') ? 1 : 0;
	}
	return s_cached == 1;
}

// Diagnostic report for one v2 mismatch: exact frame, per-subsystem
// classification, and nearby checkpoint hashes. Never touches the legacy
// snapshot state. Termination only in strict mode, only between players.
static void ReportHashMismatch(
	SessionClient* client,
	const fSystem::HashCheckpoint& local,
	const sf4e::SessionProtocol::BattleHashV2& remote
) {
	if (local.hashes.overall == remote.overall) {
		return;
	}
	spdlog::error(
		"Desync v2: mismatch @ frame {} overall {:016x} != remote {:016x} (fromPlayer={}) fp={}",
		local.frameIdx, local.hashes.overall, remote.overall, remote.fromPlayer, sf4e::statehash::FpEnvironment()
	);
	spdlog::error(
		"Desync v2: subsystems flow:{} chara0:{} chara1:{}",
		local.hashes.flow == remote.flow ? "match" : "MISMATCH",
		local.hashes.chara[0] == remote.chara0 ? "match" : "MISMATCH",
		local.hashes.chara[1] == remote.chara1 ? "match" : "MISMATCH"
	);
	spdlog::error("Desync v2: local checkpoint ggpo={} engine={} flow={} substate={}",
		local.ggpoStateFrame, local.engineFrame, local.flow, local.substate);
	fSystem::LogFrameTrace(local.frameIdx);
	for (int i = 0; i < fSystem::NUM_HASH_CHECKPOINTS; i++) {
		const fSystem::HashCheckpoint& cp = fSystem::hashCheckpoints[i];
		if (!cp.valid || cp.frameIdx == local.frameIdx) {
			continue;
		}
		int distance = cp.frameIdx - local.frameIdx;
		if (distance >= -90 && distance <= 90) {
			spdlog::error(
				"Desync v2: nearby frame {} overall {:016x} flow {:016x} c0 {:016x} c1 {:016x} engine={} nativeFlow={} substate={}",
				cp.frameIdx, cp.hashes.overall, cp.hashes.flow,
				cp.hashes.chara[0], cp.hashes.chara[1], cp.engineFrame, cp.flow, cp.substate
			);
		}
	}

	// A spectator's mismatch (either side) must never terminate the two
	// players' fight; strict termination requires both peers to be players.
	if (StrictDesyncEnabled() && remote.fromPlayer && client->IsLocalPlayer()) {
		spdlog::error("Desync v2 (strict): terminating match at frame {}", local.frameIdx);
		client->TerminateOnDesync("v2_strict", local.frameIdx);
	}
}

// Ends the fight after a confirmed state divergence, and tells the player
// why: without this the game simply returned to the menu and the table
// paused thirty seconds later. Only a player may end the fight; a spectator
// that diverged is a spectator problem and is logged only.
void SessionClient::TerminateOnDesync(const char* stage, int frameIdx) {
	if (!IsLocalPlayer()) {
		spdlog::error("Desync ({}): local spectator diverged at frame {}; the players' fight continues", stage, frameIdx);
		sf4e::NetplayFacade::PushDesyncNotice(true);
		return;
	}
	spdlog::error("Desync ({}): terminating match at frame {}", stage, frameIdx);
	sf4e::NetplayFacade::PushDesyncNotice(false);
	*rSystem::GetReadyState(rSystem::staticMethods.GetSingleton()) = rSystem::RS_ISLEAVING;
}

bool SessionClient::HandleBattleSnapshot(json& msg) {
	SessionProtocol::BattleSnapshot m;
	try {
		msg.get_to(m);
	}
	catch (const json::exception&) {
		spdlog::info("Client: could not deserialize incoming checksum msg");
		return true;
	}

	auto localSnapshotIter = fSystem::snapshotMap.find(m.snapshot.frameIdx);
	if (localSnapshotIter != fSystem::snapshotMap.end()) {
		// This client is ahead and already has a snapshot for this frame.
		// Compare it.
		SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
		if (bVerboseLogging) {
			spdlog::error("Client: snapshot receipt: valid snapshot @ frame {} on receipt, confirm {}, sent {}", localSnapshot.frameIdx, localSnapshotIter->second.second.confirmed, localSnapshotIter->second.second.sent);
		}
		if (memcmp(&m.snapshot, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
			spdlog::error("Client: snapshot receipt: Desync detected!");
			ReportSnapshotDivergence("receipt", localSnapshot, m.snapshot);
			TerminateOnDesync("receipt", localSnapshot.frameIdx);
		}

		if (bVerboseLogging) {
			spdlog::error("    - Client: snapshot receipt: valid snapshot @ frame {} on receipt confirmed", localSnapshot.frameIdx);
		}
		localSnapshotIter->second.second.confirmed = true;
		if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {

			if (bVerboseLogging) {
				spdlog::error("Client: snapshot receipt: erasing local snapshot @ frame {} on receipt due to confirmation+sent", m.snapshot.frameIdx);
			}
			fSystem::snapshotMap.erase(localSnapshotIter);
		}
	}
	else {
		// Opponent's ahead- can't compare yet.
		if (bVerboseLogging) {
			spdlog::error("Client: snapshot receipt: pendingRemoteSnapshots.emplace({})", m.snapshot.frameIdx);
		}
		if (pendingRemoteSnapshots.size() >= MAX_PENDING_REMOTE_SNAPSHOTS) {
			pendingRemoteSnapshots.erase(pendingRemoteSnapshots.begin());
		}
		pendingRemoteSnapshots.emplace(m.snapshot.frameIdx, m.snapshot);
	}
	return true;
}

bool SessionClient::HandleBattleHash(json& msg) {
	SessionProtocol::BattleHashV2 m;
	try {
		msg.get_to(m);
	}
	catch (json::exception&) {
		spdlog::info("Client: could not deserialize v2 hash msg");
		return true;
	}
	// Always buffer; the aged reconcile pass below compares only
	// once the LOCAL checkpoint is also non-speculative (a local
	// value inside the rollback window could still change).
	if (pendingRemoteHashes.size() >= MAX_PENDING_REMOTE_HASHES) {
		pendingRemoteHashes.erase(pendingRemoteHashes.begin());
	}
	pendingRemoteHashes[m.frameIdx] = m;
	return true;
}

// Send all our outstanding local snapshots and compare any to pending
// snapshots.
void SessionClient::StepBattleVerification() {
	int mostRecentPredictiveFrame = (
		rSystem::GetNumFramesSimulated_FixedPoint(rSystem::staticMethods.GetSingleton())->integral
	);
	auto localSnapshotIter = fSystem::snapshotMap.begin();
	while (localSnapshotIter != fSystem::snapshotMap.end()) {
		if (mostRecentPredictiveFrame - localSnapshotIter->first < 60) {
			// Snapshot not yet old enough.
			localSnapshotIter++;
			continue;
		}

		if (bVerboseLogging) {
			spdlog::error("Client: snapshot reconciliation: checking snapshot @ {} due to mostRecentPredictiveFrame {}", localSnapshotIter->first, mostRecentPredictiveFrame);
		}

		if (!localSnapshotIter->second.second.sent) {
			if (bVerboseLogging) {
				spdlog::error("Client: snapshot reconciliation: snapshot @ {} not yet sent, confirmed val: {}", localSnapshotIter->first, localSnapshotIter->second.second.confirmed);
			}
			// Snapshot not yet sent. Send it. Only a player's snapshot is
			// authoritative; a spectator compares but never publishes, the
			// same rule the v2 hash exchange already follows.
			localSnapshotIter->second.second.sent = true;
			if (IsLocalPlayer()) {
				SessionProtocol::BattleSnapshot m;
				m.snapshot = localSnapshotIter->second.first;
				json msg = m;
				const bool failed = Send(msg, nullptr) != session::SendResult::Queued;
				if (failed != _snapshotSendFailing)
					spdlog::log(failed ? spdlog::level::err : spdlog::level::info, failed ?
						"Client: Could not send snapshot update; further failures are not logged until one succeeds" :
						"Client: Snapshot updates are sending again");
				_snapshotSendFailing = failed;
			}
		}

		if (!localSnapshotIter->second.second.confirmed) {
			if (bVerboseLogging) {
				spdlog::error("Client: snapshot reconciliation: snapshot @ {} not yet confirmed, sent val: {}", localSnapshotIter->first, localSnapshotIter->second.second.sent);
			}
			auto remoteSnapshotIter = pendingRemoteSnapshots.find(localSnapshotIter->first);
			if (remoteSnapshotIter != pendingRemoteSnapshots.end()) {
				if (bVerboseLogging) {
					spdlog::error("   - Client: snapshot reconciliation: snapshot @ {} got candidate remote snapshot", localSnapshotIter->first);
				}
				// Caught up to the opponent- compare to a snapshot already sent by the opponent.
				SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
				if (memcmp(&remoteSnapshotIter->second, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
					spdlog::error("Client: snapshot reconciliation: Desync detected from pending!");
					ReportSnapshotDivergence("reconciliation", localSnapshot, remoteSnapshotIter->second);
					TerminateOnDesync("reconciliation", localSnapshot.frameIdx);
				}
				localSnapshotIter->second.second.confirmed = true;
				pendingRemoteSnapshots.erase(remoteSnapshotIter);
			}
		}

		if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {
			if (bVerboseLogging) {
				spdlog::error("   - Client: snapshot reconciliation: snapshot @ {} erased due to being confirmed and sent", localSnapshotIter->first);
			}
			localSnapshotIter = fSystem::snapshotMap.erase(localSnapshotIter);
		}
		else {
			localSnapshotIter++;
		}
	}

	// Read-only game-thread query. No polling or pacing changes here.
	// Only players send; spectators compare the confirmed source stream.
	{
		const bool isPlayer = IsLocalPlayer();
		int confirmedInput = -1;
		if (fSystem::ggpo) ggpo_get_last_confirmed_frame(fSystem::ggpo, &confirmedInput);
		for (int i = 0; i < fSystem::NUM_HASH_CHECKPOINTS; i++) {
			fSystem::HashCheckpoint& cp = fSystem::hashCheckpoints[i];
			if (!cp.valid) {
				continue;
			}
			if (!sf4e::statehash::IsConfirmedCheckpoint(cp.ggpoStateFrame, confirmedInput)) {
				continue;
			}
			if (!cp.sent && isPlayer) {
				SessionProtocol::BattleHashV2 m;
				m.frameIdx = cp.frameIdx;
				m.overall = cp.hashes.overall;
				m.flow = cp.hashes.flow;
				m.chara0 = cp.hashes.chara[0];
				m.chara1 = cp.hashes.chara[1];
				m.fromPlayer = true;
				json hashMsg = m;
				if (Send(hashMsg, nullptr) == session::SendResult::Queued) {
					cp.sent = true;
				}
			}
		}
		sf4e::statehash::ReconcileRemoteHashes(pendingRemoteHashes, confirmedInput, fSystem::FindHashCheckpoint,
			[this](const fSystem::HashCheckpoint& local, const SessionProtocol::BattleHashV2& remote) {
				ReportHashMismatch(this, local, remote);
			});
	}
}
