#pragma once
// Shared by the room authority suites: the checks and the members they seat.
#include "../session/RoomModel.hxx"
#include "../session/RoomCommit.hxx"
#include "../common/MatchSide.hxx"

#include <cstdio>
#include <cstdlib>
#include <algorithm>

#include <nlohmann/json.hpp>

using namespace sf4e::room;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static ConnectionRef Peer(int index) {
	return ConnectionRef{"host", std::to_string(index)};
}

static Action TableAction(const RoomAuthority& authority, MemberId member, std::uint8_t table, ActionKind kind) {
	Action action;
	action.kind = kind;
	action.roomEpoch = authority.SnapshotView().roomEpoch;
	action.revision = authority.SnapshotView().revision;
	action.table = table;
	action.tableRevision = authority.SnapshotView().tables[table].revision;
	action.actionId = member * 1000 + authority.SnapshotView().revision + 1;
	return action;
}

static MemberId Join(RoomAuthority& authority, int index, bool host = false) {
	const auto result = authority.Join("Player" + std::to_string(index), Peer(index), host);
	CHECK(result.accepted);
	return result.accepted ? result.snapshot.members.back().id : 0;
}
